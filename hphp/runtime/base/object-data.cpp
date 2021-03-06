/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/object-data.h"

#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/externals.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/req-containers.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/mixed-array-defs.h"

#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/ext/simplexml/ext_simplexml.h"
#include "hphp/runtime/ext/datetime/ext_datetime.h"
#include "hphp/runtime/ext/std/ext_std_closure.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/member-operations.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/native-prop-handler.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/repo.h"
#include "hphp/runtime/vm/repo-global-data.h"

#include "hphp/system/systemlib.h"

#include <folly/Hash.h>
#include <folly/ScopeGuard.h>

#include <vector>

namespace HPHP {

//////////////////////////////////////////////////////////////////////

// current maximum object identifier
__thread uint32_t ObjectData::os_max_id;

TRACE_SET_MOD(runtime);

const StaticString
  s_offsetGet("offsetGet"),
  s_call("__call"),
  s_clone("__clone");

static Array convert_to_array(const ObjectData* obj, Class* cls) {
  auto const prop = obj->getProp(cls, s_storage.get());

  // We currently do not special case ArrayObjects / ArrayIterators in
  // reflectionClass. Until, either ArrayObject moves to HNI or a special
  // case is added to reflection unset should be turned off.
  assertx(prop.is_set() /* && prop.type() != KindOfUninit */);
  return tvCastToArrayLike(prop.tv());
}

#ifdef _MSC_VER
static_assert(sizeof(ObjectData) == (use_lowptr ? 16 : 20),
              "Change this only on purpose");
#else
static_assert(sizeof(ObjectData) == (use_lowptr ? 16 : 24),
              "Change this only on purpose");
#endif

//////////////////////////////////////////////////////////////////////

ALWAYS_INLINE
static void invoke_destructor(ObjectData* obj, const Func* dtor) {
  try {
    // Call the destructor method
    g_context->invokeMethodV(obj, dtor, InvokeArgs{}, false);
  } catch (...) {
    // Swallow any exceptions that escape the __destruct method
    handle_destructor_exception();
  }
}

NEVER_INLINE bool ObjectData::destructImpl() {
  setNoDestruct();
  auto const dtor = m_cls->getDtor();
  if (!dtor) return true;

  // We don't run PHP destructors while we're unwinding for a C++
  // exception.  We want to minimize the PHP code we run while propagating
  // fatals, so we do this check here on a very common path, in the
  // relatively slower case.
  if (g_context->m_unwindingCppException) return true;

  // Some decref paths call release() when --count == 0 and some call it when
  // count == 1. This difference only matters for objects that resurrect
  // themselves in their destructors, so make sure count is consistent here.
  assertx(m_count == 0 || m_count == 1);
  m_count = static_cast<RefCount>(0);

  // We raise the refcount around the call to __destruct(). This is to prevent
  // the refcount from going to zero when the destructor returns.
  CountableHelper h(this);
  invoke_destructor(this, dtor);
  return hasExactlyOneRef();
}

void ObjectData::destructForExit() {
  assertx(RuntimeOption::EnableObjDestructCall);
  auto const dtor = m_cls->getDtor();
  if (dtor) {
    g_context->m_liveBCObjs.erase(this);
  }

  if (noDestruct()) return;
  setNoDestruct();

  // We're exiting, so there should not be any live faults.
  assertx(g_context->m_faults.empty());
  assertx(!g_context->m_unwindingCppException);

  CountableHelper h(this);
  invoke_destructor(this, dtor);
}

NEVER_INLINE
static void freeDynPropArray(ObjectData* inst) {
  auto& table = g_context->dynPropTable;
  auto it = table.find(inst);
  assertx(it != end(table));
  assertx(it->second.arr().isPHPArray());
  it->second.destroy();
  table.erase(it);
}

NEVER_INLINE
void ObjectData::releaseNoObjDestructCheck() noexcept {
  assertx(kindIsValid());

  // Destructors are unsupported in one-bit reference counting mode.
  if (!one_bit_refcount && UNLIKELY(!getAttribute(NoDestructor))) {
    if (UNLIKELY(!destructImpl())) return;
  }

  auto const cls = getVMClass();

  if (UNLIKELY(hasInstanceDtor())) {
    return cls->instanceDtor()(this, cls);
  }

  // `this' is being torn down now---be careful about where/how you dereference
  // this from here on.

  auto const nProps = size_t{cls->numDeclProperties()};
  auto prop = reinterpret_cast<TypedValue*>(this + 1);
  auto const stop = prop + nProps;
  for (; prop != stop; ++prop) {
    tvDecRefGen(prop);
  }

  // Deliberately reload `attrs' to check for dynamic properties.  This made
  // gcc generate better code at the time it was done (saving a spill).
  if (UNLIKELY(getAttribute(HasDynPropArr))) freeDynPropArray(this);

  auto& pmax = os_max_id;
  if (o_id && o_id == pmax) --pmax;

  invalidateWeakRef();
  auto const size =
    reinterpret_cast<char*>(stop) - reinterpret_cast<char*>(this);
  assertx(size == sizeForNProps(nProps));
  tl_heap->objFree(this, size);
  AARCH64_WALKABLE_FRAME();
}

NEVER_INLINE
static void tail_call_remove_live_bc_obj(ObjectData* obj) {
  g_context->m_liveBCObjs.erase(obj);
  return obj->releaseNoObjDestructCheck();
}

void ObjectData::release() noexcept {
  assertx(kindIsValid());
  if (UNLIKELY(RuntimeOption::EnableObjDestructCall && m_cls->getDtor())) {
    tail_call_remove_live_bc_obj(this);
    AARCH64_WALKABLE_FRAME();
    return;
  }
  releaseNoObjDestructCheck();
  AARCH64_WALKABLE_FRAME();
}

///////////////////////////////////////////////////////////////////////////////
// class info

StrNR ObjectData::getClassName() const {
  return m_cls->preClass()->nameStr();
}

bool ObjectData::instanceof(const String& s) const {
  assertx(kindIsValid());
  auto const cls = Unit::lookupClass(s.get());
  return cls && instanceof(cls);
}

bool ObjectData::toBooleanImpl() const noexcept {
  // Note: if you add more cases here, hhbbc/class-util.cpp also needs
  // to be changed.
  if (isCollection()) {
    return collections::toBool(this);
  }

  if (instanceof(SimpleXMLElement_classof())) {
    // SimpleXMLElement is the only non-collection class that has custom bool
    // casting.
    return SimpleXMLElement_objectCast(this, KindOfBoolean).toBoolean();
  }

  always_assert(false);
  return false;
}

int64_t ObjectData::toInt64Impl() const noexcept {
  // SimpleXMLElement is the only class that has proper custom int casting.
  assertx(instanceof(SimpleXMLElement_classof()));
  return SimpleXMLElement_objectCast(this, KindOfInt64).toInt64();
}

double ObjectData::toDoubleImpl() const noexcept {
  // SimpleXMLElement is the only class that has custom double casting.
  assertx(instanceof(SimpleXMLElement_classof()));
  return SimpleXMLElement_objectCast(this, KindOfDouble).toDouble();
}

///////////////////////////////////////////////////////////////////////////////
// instance methods and properties

const StaticString s_getIterator("getIterator");

Object ObjectData::iterableObject(bool& isIterable,
                                  bool mayImplementIterator /* = true */) {
  assertx(mayImplementIterator || !isIterator());
  if (mayImplementIterator && isIterator()) {
    isIterable = true;
    return Object(this);
  }
  Object obj(this);
  while (obj->instanceof(SystemLib::s_IteratorAggregateClass)) {
    auto iterator = obj->o_invoke_few_args(s_getIterator, 0);
    if (!iterator.isObject()) break;
    auto o = iterator.getObjectData();
    if (o->isIterator()) {
      isIterable = true;
      return Object{o};
    }
    obj.reset(o);
  }
  if (!isIterator() && obj->instanceof(SimpleXMLElement_classof())) {
    isIterable = true;
    return create_object(
      s_SimpleXMLElementIterator,
      make_packed_array(obj)
    );
  }
  isIterable = false;
  return obj;
}

Array& ObjectData::dynPropArray() const {
  assertx(getAttribute(HasDynPropArr));
  assertx(g_context->dynPropTable.count(this));
  assertx(g_context->dynPropTable[this].arr().isPHPArray());
  return g_context->dynPropTable[this].arr();
}

Array& ObjectData::reserveProperties(int numDynamic /* = 2 */) {
  if (getAttribute(HasDynPropArr)) {
    return dynPropArray();
  }

  return
    setDynPropArray(Array::attach(MixedArray::MakeReserveMixed(numDynamic)));
}

Array& ObjectData::setDynPropArray(const Array& newArr) {
  assertx(!g_context->dynPropTable.count(this));
  assertx(!getAttribute(HasDynPropArr));
  assertx(newArr.isPHPArray());

  if (m_cls->forbidsDynamicProps()) {
    throw_object_forbids_dynamic_props(getClassName().data());
  }

  auto& arr = g_context->dynPropTable[this].arr();
  assertx(arr.isPHPArray());
  arr = newArr;
  setAttribute(HasDynPropArr);
  return arr;
}

template<typename K>
TypedValue* ObjectData::makeDynProp(K key, AccessFlags flags) {
  SuppressHackArrCompatNotices shacn;
  return reserveProperties().lvalAt(key, flags).tv_ptr();
}

Variant ObjectData::o_get(const String& propName, bool error /* = true */,
                          const String& context /*= null_string*/) {
  assertx(kindIsValid());

  // This is not (just) a check for empty string; property names that start
  // with null are intentionally being rejected here.
  if (UNLIKELY(!*propName.data())) {
    throw_invalid_property_name(propName);
  }

  Class* ctx = nullptr;
  if (!context.empty()) {
    ctx = Unit::lookupClass(context.get());
  }

  // Can't use propImpl here because if the property is not accessible and
  // there is no magic __get, propImpl will raise_error("Cannot access ...",
  // but o_get will only (maybe) raise_notice("Undefined property ..." :-(

  auto const prop = getProp(ctx, propName.get());
  if (prop && prop.type() != KindOfUninit) {
    return Variant::wrap(tvToCell(prop.tv()));
  }

  if (m_cls->rtAttribute(Class::UseGet)) {
    if (auto r = invokeGet(propName.get())) {
      return std::move(tvAsVariant(&r.val));
    }
  }

  if (error) {
    raise_notice("Undefined property: %s::$%s", getClassName().data(),
                 propName.data());
  }

  return uninit_null();
}

void ObjectData::o_set(const String& propName, const Variant& v,
                       const String& context /* = null_string */) {
  assertx(kindIsValid());

  // This is not (just) a check for empty string; property names that start
  // with null are intentionally being rejected here.
  if (UNLIKELY(!*propName.data())) {
    throw_invalid_property_name(propName);
  }

  Class* ctx = nullptr;
  if (!context.empty()) {
    ctx = Unit::lookupClass(context.get());
  }

  // Can't use setProp here because if the property is not accessible and
  // there is no magic __set, setProp will raise_error("Cannot access ...",
  // but o_set will skip writing and return normally. Also, if we try to
  // invoke __set and fail due to recursion, setProp will fall back to writing
  // the property normally, but o_set will just skip writing and return :-(

  bool useSet = m_cls->rtAttribute(Class::UseSet);

  auto const lookup = getPropImpl<true>(ctx, propName.get());
  auto prop = lookup.prop;
  if (prop && lookup.accessible) {
    if (!useSet || prop->m_type != KindOfUninit) {
      if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
        throwMutateImmutable(prop);
      }
      tvSet(tvToInitCell(*v.asTypedValue()), *prop);
      return;
    }
  }

  if (useSet) {
    invokeSet(propName.get(), *v.asCell());
  } else if (!prop) {
    reserveProperties().set(propName, tvToInitCell(*v.asTypedValue()), true);
  }
}

void ObjectData::o_setArray(const Array& properties) {
  for (ArrayIter iter(properties); iter; ++iter) {
    String k = iter.first().toString();
    Class* ctx = nullptr;
    // If the key begins with a NUL, it's a private or protected property. Read
    // the class name from between the two NUL bytes.
    //
    // Note: if you change this, you need to change similar logic in
    // apc-object.
    if (!k.empty() && k[0] == '\0') {
      int subLen = k.find('\0', 1) + 1;
      String cls = k.substr(1, subLen - 2);
      if (cls.size() == 1 && cls[0] == '*') {
        // Protected.
        ctx = m_cls;
      } else {
        // Private.
        ctx = Unit::lookupClass(cls.get());
        if (!ctx) continue;
      }
      k = k.substr(subLen);
    }

    setProp(ctx, k.get(), tvAssertCell(iter.secondRval().tv()));
  }
}

void ObjectData::o_getArray(Array& props, bool pubOnly /* = false */) const {
  assertx(kindIsValid());

  // Fast path for classes with no declared properties
  if (!m_cls->numDeclProperties() && getAttribute(HasDynPropArr)) {
    props = dynPropArray();
    return;
  }
  // The declared properties in the resultant array should be a permutation of
  // propVec. They appear in the following order: go most-to-least-derived in
  // the inheritance hierarchy, inserting properties in declaration order (with
  // the wrinkle that overridden properties should appear only once, with the
  // access level given to it in its most-derived declaration).

  // This is needed to keep track of which elements have been inserted. This is
  // the smoothest way to get overridden properties right.
  std::vector<bool> inserted(m_cls->numDeclProperties(), false);

  // Iterate over declared properties and insert {mangled name --> prop} pairs.
  const Class* cls = m_cls;
  do {
    getProps(cls, pubOnly, cls->preClass(), props, inserted);
    for (auto const& traitCls : cls->usedTraitClasses()) {
      getTraitProps(cls, pubOnly, traitCls.get(), props, inserted);
    }
    cls = cls->parent();
  } while (cls);

  // Iterate over dynamic properties and insert {name --> prop} pairs.
  if (UNLIKELY(getAttribute(HasDynPropArr))) {
    auto& dynProps = dynPropArray();
    if (!dynProps.empty()) {
      for (ArrayIter it(dynProps.get()); !it.end(); it.next()) {
        props.setWithRef(it.first(), it.secondVal(), true);
      }
    }
  }
}

// a constant for arrayobjects that changes the way the array is
// converted to an object
const int64_t ARRAYOBJ_STD_PROP_LIST = 1;

const StaticString s_flags("flags");

Array ObjectData::toArray(bool pubOnly /* = false */) const {
  assertx(kindIsValid());

  // We can quickly tell if this object is a collection, which lets us avoid
  // checking for each class in turn if it's not one.
  if (isCollection()) {
    return collections::toArray(this);
  } else if (UNLIKELY(m_cls->rtAttribute(Class::CallToImpl))) {
    // If we end up with other classes that need special behavior, turn the
    // assert into an if and add cases.
    assertx(instanceof(SimpleXMLElement_classof()));
    return SimpleXMLElement_objectCast(this, KindOfArray).toArray();
  } else if (UNLIKELY(instanceof(SystemLib::s_ArrayObjectClass))) {
    auto const flags = getProp(SystemLib::s_ArrayObjectClass, s_flags.get());
    assertx(flags.is_set());

    if (UNLIKELY(flags.type() == KindOfInt64 &&
                 flags.val().num == ARRAYOBJ_STD_PROP_LIST)) {
      auto ret = Array::Create();
      o_getArray(ret, true);
      return ret;
    }
    return convert_to_array(this, SystemLib::s_ArrayObjectClass);
  } else if (UNLIKELY(instanceof(SystemLib::s_ArrayIteratorClass))) {
    return convert_to_array(this, SystemLib::s_ArrayIteratorClass);
  } else if (UNLIKELY(instanceof(c_Closure::classof()))) {
    return Array::Create(Object(const_cast<ObjectData*>(this)));
  } else if (UNLIKELY(instanceof(DateTimeData::getClass()))) {
    return Native::data<DateTimeData>(this)->getDebugInfo();
  } else {
    auto ret = Array::Create();
    o_getArray(ret, pubOnly);
    return ret;
  }
}

namespace {

size_t getPropertyIfAccessible(ObjectData* obj,
                               const Class* ctx,
                               const StringData* key,
                               ObjectData::IterMode mode,
                               Array& properties,
                               size_t propLeft) {
  if (mode == ObjectData::CreateRefs) {
    auto const prop = obj->vGetProp(ctx, key);
    if (prop) {
      --propLeft;
      properties.setRef(StrNR(key), tvAsVariant(prop.tv_ptr()), true);
    }
  } else {
    auto const prop = obj->getProp(ctx, key);
    if (prop && prop.type() != KindOfUninit) {
      --propLeft;
      if (mode == ObjectData::EraseRefs) {
        properties.set(StrNR(key), prop.tv(), true);
      } else {
        properties.setWithRef(StrNR(key), prop.tv(), true);
      }
    }
  }
  return propLeft;
}

}

Array ObjectData::o_toIterArray(const String& context, IterMode mode) {
  if (mode == PreserveRefs && !m_cls->numDeclProperties()) {
    if (getAttribute(HasDynPropArr)) return dynPropArray();
    return Array::Create();
  }

  Array* dynProps = nullptr;
  size_t accessibleProps = m_cls->declPropNumAccessible();
  size_t size = accessibleProps;
  if (getAttribute(HasDynPropArr)) {
    dynProps = &dynPropArray();
    size += dynProps->size();
  }
  Array retArray { Array::attach(MixedArray::MakeReserveMixed(size)) };

  Class* ctx = nullptr;
  if (!context.empty()) {
    ctx = Unit::lookupClass(context.get());
  }

  // Get all declared properties first, bottom-to-top in the inheritance
  // hierarchy, in declaration order.
  const Class* klass = m_cls;
  while (klass) {
    const PreClass::Prop* props = klass->preClass()->properties();
    const size_t numProps = klass->preClass()->numProperties();

    for (size_t i = 0; i < numProps; ++i) {
      auto key = const_cast<StringData*>(props[i].name());
      accessibleProps = getPropertyIfAccessible(
          this, ctx, key, mode, retArray, accessibleProps);
    }
    klass = klass->parent();
  }
  if (!(m_cls->attrs() & AttrNoExpandTrait) && accessibleProps > 0) {
    // we may have properties from traits
    for (auto const& prop : m_cls->declProperties()) {
      auto const key = prop.name.get();
      if (!retArray.get()->exists(key)) {
        accessibleProps = getPropertyIfAccessible(
          this, ctx, key, mode, retArray, accessibleProps);
        if (accessibleProps == 0) break;
      }
    }
  }

  // Now get dynamic properties.
  if (dynProps) {
    auto ad = dynProps->get();
    ssize_t iter = ad->iter_begin();
    auto pos_limit = ad->iter_end();
    while (iter != pos_limit) {
      auto const key = dynProps->get()->nvGetKey(iter);
      iter = dynProps->get()->iter_advance(iter);

      // You can get this if you cast an array to object. These
      // properties must be dynamic because you can't declare a
      // property with a non-string name.
      if (UNLIKELY(!isStringType(key.m_type))) {
        assertx(key.m_type == KindOfInt64);
        switch (mode) {
        case CreateRefs: {
          auto& lval = tvAsVariant(dynProps->lvalAt(key.m_data.num).tv_ptr());
          retArray.setRef(key.m_data.num, lval);
          break;
        }
        case EraseRefs: {
          auto const val = dynProps->get()->at(key.m_data.num);
          retArray.set(key.m_data.num, val);
          break;
        }
        case PreserveRefs: {
          auto const val = dynProps->get()->at(key.m_data.num);
          retArray.setWithRef(key.m_data.num, val);
          break;
        }
        }
        continue;
      }

      auto const strKey = key.m_data.pstr;
      switch (mode) {
      case CreateRefs: {
        auto& lval = tvAsVariant(
          dynProps->lvalAt(StrNR(strKey), AccessFlags::Key).tv_ptr()
        );
        retArray.setRef(StrNR(strKey), lval, true /* isKey */);
        break;
      }
      case EraseRefs: {
        auto const val = dynProps->get()->at(strKey);
        retArray.set(StrNR(strKey), val, true /* isKey */);
        break;
      }
      case PreserveRefs: {
        auto const val = dynProps->get()->at(strKey);
        retArray.setWithRef(make_tv<KindOfString>(strKey),
                            val, true /* isKey */);
        break;
      }
      }
      decRefStr(strKey);
    }
  }

  return retArray;
}

static bool decode_invoke(const String& s, ObjectData* obj, bool fatal,
                          CallCtx& ctx) {
  ctx.this_ = obj;
  ctx.cls = obj->getVMClass();
  ctx.invName = nullptr;
  ctx.dynamic = true;

  ctx.func = ctx.cls->lookupMethod(s.get());
  if (ctx.func) {
    // Null out this_ for statically called methods
    if (ctx.func->isStaticInPrologue()) {
      ctx.this_ = nullptr;
    }
  } else {
    // If this_ is non-null AND we could not find a method, try
    // looking up __call in cls's method table
    ctx.func = ctx.cls->lookupMethod(s_call.get());

    if (!ctx.func) {
      // Bail if we couldn't find the method or __call
      o_invoke_failed(ctx.cls->name()->data(), s.data(), fatal);
      return false;
    }
    // We found __call! Stash the original name into invName.
    assertx(!(ctx.func->attrs() & AttrStatic));
    ctx.invName = s.get();
    ctx.invName->incRefCount();
    ctx.dynamic = false;
  }
  return true;
}

Variant ObjectData::o_invoke(const String& s, const Variant& params,
                             bool fatal /* = true */) {
  CallCtx ctx;
  if (!decode_invoke(s, this, fatal, ctx) ||
      (!isContainer(params) && !params.isNull())) {
    return Variant(Variant::NullInit());
  }
  return Variant::attach(
    g_context->invokeFunc(ctx, params)
  );
}

#define INVOKE_FEW_ARGS_IMPL3                        \
  const Variant& a0, const Variant& a1, const Variant& a2
#define INVOKE_FEW_ARGS_IMPL6                        \
  INVOKE_FEW_ARGS_IMPL3,                             \
  const Variant& a3, const Variant& a4, const Variant& a5
#define INVOKE_FEW_ARGS_IMPL10                       \
  INVOKE_FEW_ARGS_IMPL6,                             \
  const Variant& a6, const Variant& a7, const Variant& a8, const Variant& a9
#define INVOKE_FEW_ARGS_IMPL_ARGS INVOKE_FEW_ARGS(IMPL,INVOKE_FEW_ARGS_COUNT)

Variant ObjectData::o_invoke_few_args(const String& s, int count,
                                      INVOKE_FEW_ARGS_IMPL_ARGS) {

  CallCtx ctx;
  if (!decode_invoke(s, this, true, ctx)) {
    return Variant(Variant::NullInit());
  }

  TypedValue args[INVOKE_FEW_ARGS_COUNT];
  switch(count) {
    default: not_implemented();
#if INVOKE_FEW_ARGS_COUNT > 6
    case 10: tvCopy(*a9.asTypedValue(), args[9]);
    case  9: tvCopy(*a8.asTypedValue(), args[8]);
    case  8: tvCopy(*a7.asTypedValue(), args[7]);
    case  7: tvCopy(*a6.asTypedValue(), args[6]);
#endif
#if INVOKE_FEW_ARGS_COUNT > 3
    case  6: tvCopy(*a5.asTypedValue(), args[5]);
    case  5: tvCopy(*a4.asTypedValue(), args[4]);
    case  4: tvCopy(*a3.asTypedValue(), args[3]);
#endif
    case  3: tvCopy(*a2.asTypedValue(), args[2]);
    case  2: tvCopy(*a1.asTypedValue(), args[1]);
    case  1: tvCopy(*a0.asTypedValue(), args[0]);
    case  0: break;
  }

  return Variant::attach(
    g_context->invokeFuncFew(ctx, count, args)
  );
}

ObjectData* ObjectData::clone() {
  if (isCppBuiltin()) {
    if (isCollection()) return collections::clone(this);
    if (instanceof(c_Closure::classof())) {
      return c_Closure::fromObject(this)->clone();
    }
    assertx(instanceof(c_Awaitable::classof()));
    // cloning WaitHandles is not allowed
    // invoke the instanceCtor to get the right sort of exception
    auto const ctor = m_cls->instanceCtor();
    ctor(m_cls);
    always_assert(false);
  }

  // clone prevents a leak if something throws before clone() returns
  Object clone;
  auto const nProps = m_cls->numDeclProperties();
  if (hasNativeData()) {
    assertx(m_cls->instanceCtor() == Native::nativeDataInstanceCtor);
    clone = Object::attach(
      Native::nativeDataInstanceCopyCtor(this, m_cls, nProps)
    );
    assertx(clone->hasExactlyOneRef());
    assertx(clone->hasInstanceDtor());
  } else {
    auto const size = sizeForNProps(nProps);
    auto const obj = new (tl_heap->objMalloc(size))
      ObjectData(m_cls, InitRaw{}, m_cls->getODAttrs());
    clone = Object::attach(obj);
    assertx(clone->hasExactlyOneRef());
    assertx(!clone->hasInstanceDtor());
  }

  auto const clonePropVec = clone->propVecForConstruct();
  auto const props = m_cls->declProperties();
  for (auto i = Slot{0}; i < nProps; i++) {
    if (UNLIKELY(props[i].attrs & AttrNoSerialize)) {
      // need to write default value, not value from instance we're cloning
      if (m_cls->pinitVec().size() > 0) {
        const Class::PropInitVec* propInitVec = m_cls->getPropData();
        cellCopy((*propInitVec)[i], clonePropVec[i]);
        if ((*propInitVec)[i].deepInit()) {
          tvIncRefGen(clonePropVec[i]);
          collections::deepCopy(&clonePropVec[i]);
        }
      } else {
        cellCopy(m_cls->declPropInit()[i], clonePropVec[i]);
      }
    } else {
      tvDupWithRef(propVec()[i], clonePropVec[i]);
    }
  }
  if (UNLIKELY(getAttribute(HasDynPropArr))) {
    clone->setAttribute(HasDynPropArr);
    g_context->dynPropTable.emplace(clone.get(), dynPropArray().get());
  }
  if (m_cls->rtAttribute(Class::HasClone)) {
    assertx(!isCppBuiltin());
    auto const method = clone->m_cls->lookupMethod(s_clone.get());
    assertx(method);
    g_context->invokeMethodV(clone.get(), method, InvokeArgs{}, false);
  }
  return clone.detach();
}

bool ObjectData::equal(const ObjectData& other) const {
  if (this == &other) return true;
  if (isCollection()) {
    return collections::equals(this, &other);
  }
  if (UNLIKELY(instanceof(SystemLib::s_DateTimeInterfaceClass) &&
               other.instanceof(SystemLib::s_DateTimeInterfaceClass))) {
    return DateTimeData::compare(this, &other) == 0;
  }
  if (getVMClass() != other.getVMClass()) return false;
  if (UNLIKELY(instanceof(SystemLib::s_ArrayObjectClass))) {
    // Compare the whole object, not just the array representation
    auto ar1 = Array::Create();
    auto ar2 = Array::Create();
    o_getArray(ar1);
    other.o_getArray(ar2);
    return ar1->equal(ar2.get(), false);
  }
  if (UNLIKELY(instanceof(c_Closure::classof()))) {
    // First comparison already proves they are different
    return false;
  }
  return toArray()->equal(other.toArray().get(), false);
}

bool ObjectData::less(const ObjectData& other) const {
  if (isCollection() || other.isCollection()) {
    throw_collection_compare_exception();
  }
  if (this == &other) return false;
  if (UNLIKELY(instanceof(SystemLib::s_DateTimeInterfaceClass) &&
               other.instanceof(SystemLib::s_DateTimeInterfaceClass))) {
    return DateTimeData::compare(this, &other) == -1;
  }
  if (UNLIKELY(instanceof(c_Closure::classof()))) {
    // First comparison already proves they are different
    return false;
  }
  if (getVMClass() != other.getVMClass()) return false;
  return toArray().less(other.toArray());
}

bool ObjectData::more(const ObjectData& other) const {
  if (isCollection() || other.isCollection()) {
    throw_collection_compare_exception();
  }
  if (this == &other) return false;
  if (UNLIKELY(instanceof(SystemLib::s_DateTimeInterfaceClass) &&
               other.instanceof(SystemLib::s_DateTimeInterfaceClass))) {
    return DateTimeData::compare(this, &other) == 1;
  }
  if (UNLIKELY(instanceof(c_Closure::classof()))) {
    // First comparison already proves they are different
    return false;
  }
  if (getVMClass() != other.getVMClass()) return false;
  return toArray().more(other.toArray());
}

int64_t ObjectData::compare(const ObjectData& other) const {
  if (isCollection() || other.isCollection()) {
    throw_collection_compare_exception();
  }
  if (this == &other) return 0;
  if (UNLIKELY(instanceof(SystemLib::s_DateTimeInterfaceClass) &&
               other.instanceof(SystemLib::s_DateTimeInterfaceClass))) {
    auto t1 = DateTimeData::getTimestamp(this);
    auto t2 = DateTimeData::getTimestamp(&other);
    return (t1 < t2) ? -1 : ((t1 > t2) ? 1 : 0);
  }
  // Return 1 for different classes to match PHP7 behavior.
  if (UNLIKELY(instanceof(c_Closure::classof()))) {
    // First comparison already proves they are different
    return 1;
  }
  if (getVMClass() != other.getVMClass()) return 1;
  return toArray().compare(other.toArray());
}

Variant ObjectData::offsetGet(Variant key) {
  assertx(instanceof(SystemLib::s_ArrayAccessClass));

  auto const method = m_cls->lookupMethod(s_offsetGet.get());
  assertx(method);

  return
    g_context->invokeMethodV(this, method, InvokeArgs(key.asCell(), 1), false);
}

///////////////////////////////////////////////////////////////////////////////

const StaticString
  s___get("__get"),
  s___set("__set"),
  s___isset("__isset"),
  s___unset("__unset"),
  s___sleep("__sleep"),
  s___toDebugDisplay("__toDebugDisplay"),
  s___wakeup("__wakeup"),
  s___debugInfo("__debugInfo");

void deepInitHelper(TypedValue* propVec, const TypedValueAux* propData,
                    size_t nProps) {
  auto dst = propVec;
  auto src = propData;
  for (; src != propData + nProps; ++src, ++dst) {
    *dst = *src;
    // m_aux.u_deepInit is true for properties that need "deep" initialization
    if (src->deepInit()) {
      tvIncRefGen(*dst);
      collections::deepCopy(dst);
    }
  }
}

// called from jit code
ObjectData* ObjectData::newInstanceRawSmall(Class* cls, size_t size,
                                          size_t index) {
  assertx(cls->getODAttrs() == DefaultAttrs);
  assertx(size <= kMaxSmallSize);
  auto mem = tl_heap->mallocSmallIndexSize(index, size);
  return new (mem) ObjectData(cls, InitRaw{}, DefaultAttrs);
}

ObjectData* ObjectData::newInstanceRawBig(Class* cls, size_t size) {
  assertx(cls->getODAttrs() == DefaultAttrs);
  auto mem = tl_heap->mallocBigSize(size);
  return new (mem) ObjectData(cls, InitRaw{}, DefaultAttrs);
}

// called from jit code
ObjectData* ObjectData::newInstanceRawAttrsSmall(Class* cls, size_t size,
                                              size_t index,
                                              uint8_t attrs) {
  assertx(size <= kMaxSmallSize);
  auto mem = tl_heap->mallocSmallIndexSize(index, size);
  return new (mem) ObjectData(cls, InitRaw{}, attrs);
}

ObjectData* ObjectData::newInstanceRawAttrsBig(Class* cls, size_t size,
                                              uint8_t attrs) {
  auto mem = tl_heap->mallocBigSize(size);
  return new (mem) ObjectData(cls, InitRaw{}, attrs);
}


// Note: the normal object destruction path does not actually call this
// destructor.  See ObjectData::release.
ObjectData::~ObjectData() {
  auto& pmax = os_max_id;
  if (o_id && o_id == pmax) {
    --pmax;
  }
  if (UNLIKELY(getAttribute(HasDynPropArr))) freeDynPropArray(this);
}

Object ObjectData::FromArray(ArrayData* properties) {
  assertx(properties->isPHPArray());
  Object retval{SystemLib::s_stdclassClass};
  retval->setAttribute(HasDynPropArr);
  g_context->dynPropTable.emplace(retval.get(), properties);
  return retval;
}

Slot ObjectData::declPropInd(const TypedValue* prop) const {
  // Do an address range check to determine whether prop physically resides
  // in propVec.
  const TypedValue* pv = propVec();
  if (prop >= pv && prop < &pv[m_cls->numDeclProperties()]) {
    return prop - pv;
  } else {
    return kInvalidSlot;
  }
}

NEVER_INLINE
void ObjectData::throwMutateImmutable(const TypedValue* prop) const {
  auto const propIdx = declPropInd(prop);
  throw_cannot_modify_immutable_prop(
    getClassName().data(),
    m_cls->declProperties()[propIdx].name->data()
  );
}

NEVER_INLINE
void ObjectData::throwBindImmutable(const TypedValue* prop) const {
  auto const propIdx = declPropInd(prop);
  throw_cannot_bind_immutable_prop(
    getClassName().data(),
    m_cls->declProperties()[propIdx].name->data()
  );
}

template <bool forWrite>
ALWAYS_INLINE
ObjectData::PropLookup<TypedValue*> ObjectData::getPropImpl(
  const Class* ctx,
  const StringData* key
) {
  auto const lookup = m_cls->getDeclPropIndex(ctx, key);
  auto const propIdx = lookup.prop;

  if (LIKELY(propIdx != kInvalidSlot)) {
    // We found a visible property, but it might not be accessible.  No need to
    // check if there is a dynamic property with this name.
    auto const prop = &propVec()[propIdx];

    if (debug) {
      if (RuntimeOption::RepoAuthoritative) {
        auto const repoTy = m_cls->declPropRepoAuthType(propIdx);
        always_assert(tvMatchesRepoAuthType(*prop, repoTy));
      }
    }

    return PropLookup<TypedValue*> {
     const_cast<TypedValue*>(prop),
     lookup.accessible,
     // we always return true in the !forWrite case; this way the compiler
     // may optimize away this value, and if a caller intends to write but
     // instantiates with false by mistake it will always see immutable
     forWrite
       ? bool(m_cls->declProperties()[propIdx].attrs & AttrIsImmutable)
       : true,
   };
  }

  // We could not find a visible declared property. We need to check for a
  // dynamic property with this name.
  if (UNLIKELY(getAttribute(HasDynPropArr))) {
    if (auto const rval = dynPropArray()->rval(key)) {
      // Returning a non-declared property. We know that it is accessible and
      // not immutable since all dynamic properties are. If we may write to
      // the property we need to allow the array to escalate.
      if (forWrite) {
        auto const lval = dynPropArray().lvalAt(StrNR(key), AccessFlags::Key);
        return PropLookup<TypedValue*> { lval.tv_ptr(), true, false };
      } else {
        return PropLookup<TypedValue*> {
          const_cast<TypedValue*>(rval.tv_ptr()),
          true,
          true,
        };
      }
    }
  }

  return PropLookup<TypedValue*> { nullptr, false, forWrite ? false : true };
}

tv_lval ObjectData::getPropLval(const Class* ctx, const StringData* key) {
  auto const lookup = getPropImpl<true>(ctx, key);
  if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
    throwMutateImmutable(lookup.prop);
  }
  return tv_lval { lookup.prop && lookup.accessible ? lookup.prop : nullptr };
}

tv_rval ObjectData::getProp(const Class* ctx, const StringData* key) const {
  auto const lookup = const_cast<ObjectData*>(this)
    ->getPropImpl<false>(ctx, key);
  return tv_rval { lookup.prop && lookup.accessible ? lookup.prop : nullptr };
}

tv_lval ObjectData::vGetProp(const Class* ctx, const StringData* key) {
  auto const lookup = getPropImpl<true>(ctx, key);
  auto prop = lookup.prop;
  if (UNLIKELY(lookup.immutable)) throwBindImmutable(prop);
  if (lookup.accessible && prop && prop->m_type != KindOfUninit) {
    tvBoxIfNeeded(*prop);
    return tv_lval { prop };
  }
  return tv_lval{};
}

tv_lval ObjectData::vGetPropIgnoreAccessibility(const StringData* key) {
  auto const lookup = getPropImpl<true>(nullptr, key);
  auto prop = lookup.prop;
  if (UNLIKELY(lookup.immutable)) throwBindImmutable(prop);
  if (prop && prop->m_type != KindOfUninit) {
    tvBoxIfNeeded(*prop);
    return tv_lval { prop };
  }
  return tv_lval{};
}

//////////////////////////////////////////////////////////////////////

inline InvokeResult::InvokeResult(bool ok, Variant&& v) :
  val(*v.asTypedValue()) {
  tvWriteUninit(*v.asTypedValue());
  val.m_aux.u_ok = ok;
}

struct PropAccessInfo {
  struct Hash;

  bool operator==(const PropAccessInfo& o) const {
    return obj == o.obj && rt_attr == o.rt_attr && key->same(o.key);
  }

  ObjectData* obj;
  const StringData* key;      // note: not necessarily static
  Class::RuntimeAttribute rt_attr;
};

struct PropAccessInfo::Hash {
  size_t operator()(PropAccessInfo const& info) const {
    return hash_int64_pair(reinterpret_cast<intptr_t>(info.obj),
                           info.key->hash() |
                           (static_cast<int64_t>(info.rt_attr) << 32));
  }
};

struct PropRecurInfo {
  using RecurSet = req::hash_set<PropAccessInfo, PropAccessInfo::Hash>;
  const PropAccessInfo* activePropInfo{nullptr};
  RecurSet* activeSet{nullptr};
};

namespace {

/*
 * Recursion of magic property accessors is allowed, but if you
 * recurse on the same object, for the same property, for the same
 * kind of magic method, it doesn't actually enter the magic method
 * anymore.  This matches zend behavior.
 *
 * This means we need to track all active property getters and ensure
 * we aren't recursing for the same one.  Since most accesses to magic
 * property getters aren't going to recurse, we optimize for the case
 * where only a single getter is active.  If it recurses again, we
 * promote to a hash set to track all the information needed.
 *
 * The various invokeFoo functions are the entry points here.  They
 * require that the appropriate ObjectData::Attribute has been checked
 * first, and return false if they refused to run the magic method due
 * to a recursion error.
 */

THREAD_LOCAL(PropRecurInfo, propRecurInfo);

template <class Invoker>
InvokeResult
magic_prop_impl(const StringData* /*key*/, const PropAccessInfo& info,
                Invoker invoker) {
  auto recur_info = propRecurInfo.get();
  if (UNLIKELY(recur_info->activePropInfo != nullptr)) {
    auto activeSet = recur_info->activeSet;
    if (!activeSet) {
      activeSet = req::make_raw<PropRecurInfo::RecurSet>();
      activeSet->insert(*recur_info->activePropInfo);
      recur_info->activeSet = activeSet;
    }
    if (!activeSet->insert(info).second) {
      // We're already running a magic method on the same type here.
      return {false, make_tv<KindOfUninit>()};
    }
    SCOPE_EXIT {
      activeSet->erase(info);
    };

    return {true, invoker()};
  }

  recur_info->activePropInfo = &info;
  SCOPE_EXIT {
    recur_info->activePropInfo = nullptr;
    auto activeSet = recur_info->activeSet;
    if (UNLIKELY(activeSet != nullptr)) {
      req::destroy_raw(activeSet);
      recur_info->activeSet = nullptr;
    }
  };

  return {true, invoker()};
}

// Helper for making invokers for the single-argument magic property
// methods.  __set takes 2 args, so it uses its own function.
struct MagicInvoker {
  const StringData* magicFuncName;
  const PropAccessInfo& info;

  TypedValue operator()() const {
    auto const meth = info.obj->getVMClass()->lookupMethod(magicFuncName);
    TypedValue args[1] = {
      make_tv<KindOfString>(const_cast<StringData*>(info.key))
    };
    return g_context->invokeMethod(info.obj, meth, folly::range(args), false);
  }
};

}

bool ObjectData::invokeSet(const StringData* key, Cell val) {
  auto const info = PropAccessInfo { this, key, Class::UseSet };
  auto r = magic_prop_impl(key, info, [&] {
    auto const meth = m_cls->lookupMethod(s___set.get());
    TypedValue args[2] = {
      make_tv<KindOfString>(const_cast<StringData*>(key)),
      val
    };
    return g_context->invokeMethod(this, meth, folly::range(args), false);
  });
  if (r) tvDecRefGen(r.val);
  return r.ok();
}

InvokeResult ObjectData::invokeGet(const StringData* key) {
  auto const info = PropAccessInfo { this, key, Class::UseGet };
  return magic_prop_impl(
    key,
    info,
    MagicInvoker { s___get.get(), info }
  );
}

InvokeResult ObjectData::invokeIsset(const StringData* key) {
  auto const info = PropAccessInfo { this, key, Class::UseIsset };
  return magic_prop_impl(
    key,
    info,
    MagicInvoker { s___isset.get(), info }
  );
}

bool ObjectData::invokeUnset(const StringData* key) {
  auto const info = PropAccessInfo { this, key, Class::UseUnset };
  auto r = magic_prop_impl(key, info,
                           MagicInvoker{s___unset.get(), info});
  if (r) tvDecRefGen(r.val);
  return r.ok();
}

static InvokeResult guardedNativePropResult(Variant result) {
  if (!Native::isPropHandled(result)) {
    return {false, make_tv<KindOfUninit>()};
  }
  return InvokeResult{true, std::move(result)};
}

InvokeResult ObjectData::invokeNativeGetProp(const StringData* key) {
  return guardedNativePropResult(
      Native::getProp(Object{this}, StrNR(key))
  );
}

bool ObjectData::invokeNativeSetProp(const StringData* key, Cell val) {
  auto r = guardedNativePropResult(
    Native::setProp(Object{this}, StrNR(key), tvAsCVarRef(&val))
  );
  tvDecRefGen(r.val);
  return r.ok();
}

InvokeResult ObjectData::invokeNativeIssetProp(const StringData* key) {
  return guardedNativePropResult(
      Native::issetProp(Object{this}, StrNR(key))
  );
}

bool ObjectData::invokeNativeUnsetProp(const StringData* key) {
  auto r = guardedNativePropResult(
      Native::unsetProp(Object{this}, StrNR(key))
  );
  tvDecRefGen(r.val);
  return r.ok();
}

//////////////////////////////////////////////////////////////////////

template<ObjectData::PropMode mode>
TypedValue* ObjectData::propImpl(TypedValue* tvRef, const Class* ctx,
                                 const StringData* key) {
  auto constexpr write = (mode == PropMode::DimForWrite) ||
                         (mode == PropMode::Bind);
  auto const lookup = getPropImpl<write>(ctx, key);
  auto const prop = lookup.prop;

  if (prop) {
    if (lookup.accessible) {
      auto const checkImmutable = [&]() {
        if (mode == PropMode::Bind) {
          if (UNLIKELY(lookup.immutable)) throwBindImmutable(prop);
        }
        if (mode == PropMode::DimForWrite) {
          if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
            throwMutateImmutable(prop);
          }
        }
        return prop;
      };

      // Property exists, is accessible, and is not unset.
      if (prop->m_type != KindOfUninit) return checkImmutable();

      // Property is unset, try __get.
      if (m_cls->rtAttribute(Class::UseGet)) {
        if (auto r = invokeGet(key)) {
          tvCopy(r.val, *tvRef);
          return tvRef;
        }
      }

      if (mode == PropMode::ReadWarn) raiseUndefProp(key);
      if (write) return checkImmutable();
      return const_cast<TypedValue*>(&immutable_null_base);
    }

    // Property is not accessible, try __get.
    if (m_cls->rtAttribute(Class::UseGet)) {
      if (auto r = invokeGet(key)) {
        tvCopy(r.val, *tvRef);
        return tvRef;
      }
    }

    // Property exists, but it is either protected or private since accessible
    // is false.
    auto const propInd = m_cls->lookupDeclProp(key);
    auto const attrs = m_cls->declProperties()[propInd].attrs;
    auto const priv = (attrs & AttrPrivate) ? "private" : "protected";

    raise_error(
      "Cannot access %s property %s::$%s",
      priv,
      m_cls->preClass()->name()->data(),
      key->data()
    );
  }

  // First see if native getter is implemented.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    if (auto r = invokeNativeGetProp(key)) {
      tvCopy(r.val, *tvRef);
      return tvRef;
    }
  }

  // Next try calling user-level `__get` if it's used.
  if (m_cls->rtAttribute(Class::UseGet)) {
    if (auto r = invokeGet(key)) {
      tvCopy(r.val, *tvRef);
      return tvRef;
    }
  }

  if (UNLIKELY(!*key->data())) {
    throw_invalid_property_name(StrNR(key));
  }

  if (mode == PropMode::ReadWarn) raiseUndefProp(key);
  if (write) return makeDynProp(StrNR(key), AccessFlags::Key);
  return const_cast<TypedValue*>(&immutable_null_base);
}

TypedValue* ObjectData::prop(
  TypedValue* tvRef,
  const Class* ctx,
  const StringData* key
) {
  return propImpl<PropMode::ReadNoWarn>(tvRef, ctx, key);
}

TypedValue* ObjectData::propW(
  TypedValue* tvRef,
  const Class* ctx,
  const StringData* key
) {
  return propImpl<PropMode::ReadWarn>(tvRef, ctx, key);
}

TypedValue* ObjectData::propD(
  TypedValue* tvRef,
  const Class* ctx,
  const StringData* key
) {
  return propImpl<PropMode::DimForWrite>(tvRef, ctx, key);
}

TypedValue* ObjectData::propB(
  TypedValue* tvRef,
  const Class* ctx,
  const StringData* key
) {
  return propImpl<PropMode::Bind>(tvRef, ctx, key);
}

bool ObjectData::propIsset(const Class* ctx, const StringData* key) {
  auto const prop = getProp(ctx, key);
  if (prop && prop.type() != KindOfUninit) {
    return prop.unboxed().type() != KindOfNull;
  }

  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    if (auto r = invokeNativeIssetProp(key)) {
      tvCastToBooleanInPlace(&r.val);
      return r.val.m_data.num;
    }
  }

  if (!m_cls->rtAttribute(Class::UseIsset)) return false;
  auto r = invokeIsset(key);
  if (!r) return false;
  tvCastToBooleanInPlace(&r.val);
  return r.val.m_data.num;
}

bool ObjectData::propEmptyImpl(const Class* ctx, const StringData* key) {
  auto const prop = getProp(ctx, key);
  if (prop && prop.type() != KindOfUninit) {
    return !cellToBool(prop.unboxed().tv());
  }

  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    if (auto r = invokeNativeIssetProp(key)) {
      tvCastToBooleanInPlace(&r.val);
      if (!r.val.m_data.num) return true;
      if (auto r2 = invokeNativeGetProp(key)) {
        auto const emptyResult = !cellToBool(*tvToCell(&r2.val));
        tvDecRefGen(&r2.val);
        return emptyResult;
      }
      return false;
    }
  }

  if (!m_cls->rtAttribute(Class::UseIsset)) return true;
  auto r = invokeIsset(key);
  if (!r) return true;

  tvCastToBooleanInPlace(&r.val);
  if (!r.val.m_data.num) return true;

  if (m_cls->rtAttribute(Class::UseGet)) {
    if (auto r = invokeGet(key)) {
      auto const emptyResult = !cellToBool(*tvToCell(&r.val));
      tvDecRefGen(&r.val);
      return emptyResult;
    }
  }
  return false;
}

bool ObjectData::propEmpty(const Class* ctx, const StringData* key) {
  if (UNLIKELY(m_cls->rtAttribute(Class::CallToImpl))) {
    // We only get here for SimpleXMLElement or collections
    if (LIKELY(!isCollection())) {
      assertx(instanceof(SimpleXMLElement_classof()));
      return SimpleXMLElement_propEmpty(this, key);
    }
  }
  return propEmptyImpl(ctx, key);
}

void ObjectData::setProp(Class* ctx, const StringData* key, Cell val) {
  auto const lookup = getPropImpl<true>(ctx, key);
  auto const prop = lookup.prop;

  if (prop && lookup.accessible) {
    if (prop->m_type != KindOfUninit ||
        !m_cls->rtAttribute(Class::UseSet) ||
        !invokeSet(key, val)) {
      if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
        throwMutateImmutable(prop);
      }
      tvSet(val, *prop);
    }
    return;
  }

  // First see if native setter is implemented.
  if (m_cls->rtAttribute(Class::HasNativePropHandler) &&
      invokeNativeSetProp(key, val)) {
    return;
  }

  // Then go to user-level `__set`.
  if (!m_cls->rtAttribute(Class::UseSet) || !invokeSet(key, val)) {
    if (prop) {
      /*
       * Note: this differs from Zend right now in the case of a
       * failed recursive __set.  In Zend, the __set is silently
       * dropped, and the protected property is not modified.
       */
      raise_error("Cannot access protected property");
    }
    if (UNLIKELY(!*key->data())) {
      throw_invalid_property_name(StrNR(key));
    }
    reserveProperties().set(StrNR(key), val, true);
    return;
  }
}

TypedValue* ObjectData::setOpProp(TypedValue& tvRef,
                                  Class* ctx,
                                  SetOpOp op,
                                  const StringData* key,
                                  Cell* val) {
  auto const lookup = getPropImpl<true>(ctx, key);
  auto prop = lookup.prop;

  if (prop && lookup.accessible) {
    if (prop->m_type == KindOfUninit && m_cls->rtAttribute(Class::UseGet)) {
      if (auto r = invokeGet(key)) {
        SCOPE_EXIT { tvDecRefGen(r.val); };
        // don't unbox until after setopBody; see longer comment below
        setopBody(tvToCell(&r.val), op, val);
        tvUnboxIfNeeded(r.val);
        if (m_cls->rtAttribute(Class::UseSet)) {
          cellDup(tvAssertCell(r.val), tvRef);
          if (invokeSet(key, tvAssertCell(tvRef))) {
            return &tvRef;
          }
          tvRef.m_type = KindOfUninit;
        }
        if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
          throwMutateImmutable(prop);
        }
        cellDup(tvAssertCell(r.val), *prop);
        return prop;
      }
    }
    if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
      throwMutateImmutable(prop);
    }
    prop = tvToCell(prop);
    setopBody(prop, op, val);
    return prop;
  }

  if (UNLIKELY(!*key->data())) throw_invalid_property_name(StrNR(key));

  // Native accessors.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    if (auto r = invokeNativeGetProp(key)) {
      tvCopy(r.val, tvRef);
      setopBody(tvToCell(&tvRef), op, val);
      if (invokeNativeSetProp(key, tvToCell(tvRef))) {
        return &tvRef;
      }
    }
    // XXX else, write tvRef = null?
  }

  auto const useSet = m_cls->rtAttribute(Class::UseSet);
  auto const useGet = m_cls->rtAttribute(Class::UseGet);

  if (useGet && !useSet) {
    auto r = invokeGet(key);
    if (!r) tvWriteNull(r.val);
    SCOPE_EXIT { tvDecRefGen(r.val); };

    // Note: the tvUnboxIfNeeded comes *after* the setop on purpose
    // here, even though it comes before the IncDecOp in the analogous
    // situation in incDecProp.  This is to match zend 5.5 behavior.
    setopBody(tvToCell(&r.val), op, val);
    tvUnboxIfNeeded(r.val);

    if (prop) raise_error("Cannot access protected property");
    prop = makeDynProp(StrNR(key), AccessFlags::Key);

    // Normally this code path is defining a new dynamic property, but
    // unlike the non-magic case below, we may have already created it
    // under the recursion into invokeGet above, so we need to do a
    // tvSet here.
    tvSet(r.val, *prop);
    return prop;
  }

  if (useGet && useSet) {
    if (auto r = invokeGet(key)) {
      // Matching zend again: incDecProp does an unbox before the
      // operation, but setop doesn't need to here.  (We'll unbox the
      // value that gets passed to the magic setter, though, since
      // __set functions can't take parameters by reference.)
      tvCopy(r.val, tvRef);
      setopBody(tvToCell(&tvRef), op, val);
      invokeSet(key, tvToCell(tvRef));
      return &tvRef;
    }
  }

  if (prop) raise_error("Cannot access protected property");

  // No visible/accessible property, and no applicable magic method:
  // create a new dynamic property.  (We know this is a new property,
  // or it would've hit the visible && accessible case above.)
  prop = makeDynProp(StrNR(key), AccessFlags::Key);
  assertx(prop->m_type == KindOfNull); // cannot exist yet
  setopBody(prop, op, val);
  return prop;
}

Cell ObjectData::incDecProp(Class* ctx, IncDecOp op, const StringData* key) {
  auto const lookup = getPropImpl<true>(ctx, key);
  auto prop = lookup.prop;

  if (prop && lookup.accessible) {
    if (prop->m_type == KindOfUninit && m_cls->rtAttribute(Class::UseGet)) {
      if (auto r = invokeGet(key)) {
        SCOPE_EXIT { tvDecRefGen(r.val); };
        tvUnboxIfNeeded(r.val);
        auto const dest = IncDecBody(op, tvAssertCell(&r.val));
        if (m_cls->rtAttribute(Class::UseSet)) {
          invokeSet(key, tvAssertCell(r.val));
          return dest;
        }
        if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
          throwMutateImmutable(prop);
        }
        cellCopy(tvAssertCell(r.val), *prop);
        tvWriteNull(r.val); // suppress decref
        return dest;
      }
    }
    if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
      throwMutateImmutable(prop);
    }
    if (prop->m_type == KindOfUninit) {
      tvWriteNull(*prop);
    } else {
      prop = tvToCell(prop);
    }
    return IncDecBody(op, tvAssertCell(prop));
  }

  if (UNLIKELY(!*key->data())) throw_invalid_property_name(StrNR(key));

  // Native accessors.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    if (auto r = invokeNativeGetProp(key)) {
      SCOPE_EXIT { tvDecRefGen(r.val); };
      tvUnboxIfNeeded(r.val);
      auto const dest = IncDecBody(op, tvAssertCell(&r.val));
      if (invokeNativeSetProp(key, tvAssertCell(r.val))) {
        return dest;
      }
    }
  }

  auto const useSet = m_cls->rtAttribute(Class::UseSet);
  auto const useGet = m_cls->rtAttribute(Class::UseGet);

  if (useGet && !useSet) {
    auto r = invokeGet(key);
    if (!r) tvWriteNull(r.val);
    SCOPE_EXIT { tvDecRefGen(r.val); };
    tvUnboxIfNeeded(r.val);
    auto const dest = IncDecBody(op, tvAssertCell(&r.val));
    if (prop) raise_error("Cannot access protected property");
    prop = makeDynProp(StrNR(key), AccessFlags::Key);

    // Normally this code path is defining a new dynamic property, but
    // unlike the non-magic case below, we may have already created it
    // under the recursion into invokeGet above, so we need to do a
    // tvSet here.
    tvSet(r.val, *prop);
    return dest;
  }

  if (useGet && useSet) {
    if (auto r = invokeGet(key)) {
      SCOPE_EXIT { tvDecRefGen(r.val); };
      tvUnboxIfNeeded(r.val);
      auto const dest = IncDecBody(op, tvAssertCell(&r.val));
      invokeSet(key, tvAssertCell(r.val));
      return dest;
    }
  }

  if (prop) raise_error("Cannot access protected property");

  // No visible/accessible property, and no applicable magic method:
  // create a new dynamic property.  (We know this is a new property,
  // or it would've hit the visible && accessible case above.)
  prop = makeDynProp(StrNR(key), AccessFlags::Key);
  assertx(prop->m_type == KindOfNull); // cannot exist yet
  return IncDecBody(op, prop);
}

void ObjectData::unsetProp(Class* ctx, const StringData* key) {
  auto const lookup = getPropImpl<true>(ctx, key);
  auto const prop = lookup.prop;

  if (prop && lookup.accessible && prop->m_type != KindOfUninit) {
    if (declPropInd(prop) != kInvalidSlot) {
      // Declared property.
      if (UNLIKELY(lookup.immutable) && !isBeingConstructed()) {
        throwMutateImmutable(prop);
      }
      tvSetIgnoreRef(*uninit_variant.asTypedValue(), *prop);
    } else {
      // Dynamic property.
      dynPropArray().remove(StrNR(key).asString(), true /* isString */);
    }
    return;
  }

  // Native unset first.
  if (m_cls->rtAttribute(Class::HasNativePropHandler) &&
      invokeNativeUnsetProp(key)) {
    return;
  }

  auto const tryUnset = m_cls->rtAttribute(Class::UseUnset);

  if (prop && !lookup.accessible && !tryUnset) {
    // Defined property that is not accessible.
    raise_error("Cannot unset inaccessible property");
  }

  if (!tryUnset || !invokeUnset(key)) {
    if (UNLIKELY(!*key->data())) {
      throw_invalid_property_name(StrNR(key));
    }
    return;
  }
}

void ObjectData::raiseObjToIntNotice(const char* clsName) {
  raise_notice("Object of class %s could not be converted to int", clsName);
}

void ObjectData::raiseObjToDoubleNotice(const char* clsName) {
  raise_notice("Object of class %s could not be converted to float", clsName);
}

void ObjectData::raiseAbstractClassError(Class* cls) {
  Attr attrs = cls->attrs();
  raise_error("Cannot instantiate %s %s",
              (attrs & AttrInterface) ? "interface" :
              (attrs & AttrTrait)     ? "trait" :
              (attrs & AttrEnum)      ? "enum" : "abstract class",
              cls->preClass()->name()->data());
}

void ObjectData::raiseUndefProp(const StringData* key) {
  raise_notice("Undefined property: %s::$%s",
               m_cls->name()->data(), key->data());
}

void ObjectData::getProp(const Class* klass,
                         bool pubOnly,
                         const PreClass::Prop* prop,
                         Array& props,
                         std::vector<bool>& inserted) const {
  if (prop->attrs()
      & (AttrStatic |    // statics aren't part of individual instances
         AttrNoSerialize // runtime-internal attrs, such as the
                         // <<__Memoize>> cache
        )) {
    return;
  }

  Slot propInd = klass->lookupDeclProp(prop->name());
  assertx(propInd != kInvalidSlot);
  const TypedValue* propVal = &propVec()[propInd];

  if ((!pubOnly || (prop->attrs() & AttrPublic)) &&
      propVal->m_type != KindOfUninit &&
      !inserted[propInd]) {
    inserted[propInd] = true;
    props.setWithRef(
      StrNR(klass->declProperties()[propInd].mangledName).asString(),
      tvAsCVarRef(propVal));
  }
}

void ObjectData::getProps(const Class* klass, bool pubOnly,
                          const PreClass* pc,
                          Array& props,
                          std::vector<bool>& inserted) const {
  PreClass::Prop const* propVec = pc->properties();
  size_t count = pc->numProperties();
  for (size_t i = 0; i < count; ++i) {
    getProp(klass, pubOnly, &propVec[i], props, inserted);
  }
}

void ObjectData::getTraitProps(const Class* klass, bool pubOnly,
                               const Class* trait, Array& props,
                               std::vector<bool>& inserted) const {
  assertx(isNormalClass(klass));
  assertx(isTrait(trait));

  getProps(klass, pubOnly, trait->preClass(), props, inserted);
  for (auto const& traitCls : trait->usedTraitClasses()) {
    getProps(klass, pubOnly, traitCls->preClass(), props, inserted);
    getTraitProps(klass, pubOnly, traitCls.get(), props, inserted);
  }
}

static Variant invokeSimple(ObjectData* obj, const StaticString& name) {
  auto const meth = obj->methodNamed(name.get());
  return meth
    ? g_context->invokeMethodV(obj, meth, InvokeArgs{}, false)
    : uninit_null();
}

Variant ObjectData::invokeSleep() {
  return invokeSimple(this, s___sleep);
}

Variant ObjectData::invokeToDebugDisplay() {
  return invokeSimple(this, s___toDebugDisplay);
}

Variant ObjectData::invokeWakeup() {
  return invokeSimple(this, s___wakeup);
}

Variant ObjectData::invokeDebugInfo() {
  return invokeSimple(this, s___debugInfo);
}

String ObjectData::invokeToString() {
  const Func* method = m_cls->getToString();
  if (!method) {
    // If the object does not define a __toString() method, raise a
    // recoverable error
    raise_recoverable_error(
      "Object of class %s could not be converted to string",
      classname_cstr()
    );
    // If the user error handler decides to allow execution to continue,
    // we return the empty string.
    return empty_string();
  }
  auto const tv = g_context->invokeMethod(this, method, InvokeArgs{}, false);
  if (!isStringType(tv.m_type)) {
    // Discard the value returned by the __toString() method and raise
    // a recoverable error
    tvDecRefGen(tv);
    raise_recoverable_error(
      "Method %s::__toString() must return a string value",
      m_cls->preClass()->name()->data());
    // If the user error handler decides to allow execution to continue,
    // we return the empty string.
    return empty_string();
  }

  return String::attach(tv.m_data.pstr);
}

bool ObjectData::hasToString() {
  return (m_cls->getToString() != nullptr);
}

const char* ObjectData::classname_cstr() const {
  return getClassName().data();
}

} // HPHP
