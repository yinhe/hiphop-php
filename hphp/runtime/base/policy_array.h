/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   | Copyright (c) 1998-2010 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
*/

#ifndef HPHP_POLICY_ARRAY_H_
#define HPHP_POLICY_ARRAY_H_

#include "hphp/runtime/base/types.h"
#include "hphp/runtime/base/array_data.h"
#include "hphp/runtime/base/smart_allocator.h"
#include "hphp/runtime/base/complex_types.h"
#include "hphp/util/trace.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/**
PosType is a int-like type that denotes a position in an array. All
traffic in ArrayStore implementation uses PosType consistently. The
advantage is that there's never a possibility to confuse a position
with e.g. an integral key. The disadvantage is that explicit casts
must be inserted whenever we have an integral and need a position, or
vice versa.
  */
enum class PosType : size_t { invalid = size_t(-1) };
static_assert(ssize_t(PosType::invalid) == ArrayData::invalid_index,
              "Bad design: can't have PosType::invalid and"
              " ArrayData::invalid_index mean distinct things.");

template<typename T>
inline T toInt(PosType pos) {
  return static_cast<T>(pos);
}

template<typename T>
inline PosType toPos(T n) {
  return static_cast<PosType>(n);
}

/**
SimpleArrayStore implements a basic storage strategy for PolicyArray. It
uses two contiguous arrays, one for keys and the other for
values. There is no indexing, just linear search, so don't use for
large arrays.

Storage policies don't store the array size internally because it's
already a member in ArrayData; keeping it here again only invites bugs
and inconsistencies. So the API requires passing the size around
whenever necessary. The name used herein is 'length' and it's a uint
because array lengths cannot be larger than that.
 */
class SimpleArrayStore {
  union Key {
    int64_t i;
    StringData* s;
  };

  // Nope
  SimpleArrayStore() = delete;
  SimpleArrayStore(const SimpleArrayStore&) = delete;
  SimpleArrayStore& operator=(const SimpleArrayStore&) = delete;

  /**
  Allocate keys and values at a given capacity and with a given
  allocation strategy.
  */
  static void allocate(Key*& ks, TypedValueAux*& vs, uint cap,
                       ArrayData::AllocationMode am) {
    /* To save time, only one allocation is done and keys are stored
       right next to the values. */
    auto const totSize = (sizeof(*ks) + sizeof(*vs)) * cap;
    auto const raw = am == ArrayData::AllocationMode::nonSmart
      ? Util::safe_malloc(totSize)
      : smart_malloc(totSize);
    vs = static_cast<TypedValueAux*>(raw);
    ks = reinterpret_cast<Key*>(vs + cap);
  }

  /** Inverse of allocate */
  static void deallocate(Key* ks, TypedValueAux* vs,
                         ArrayData::AllocationMode am) {
    assert(ks && vs);
    if (am == ArrayData::AllocationMode::nonSmart) {
      Util::safe_free(vs);
    } else {
      smart_free(vs);
    }
  }

protected:
  /**
  This is the smallest non-zero capacity allocated.
  */
  enum : unsigned int { startingCapacity = 16 };

  /**
  Returns true iff pos1 comes before pos2 in the array's normal order.
  */
  static bool before(ssize_t pos1, PosType pos2) {
    return pos1 < ssize_t(pos2);
  }

  /**
  Pseudo-copy constructor taking the source, the needed length and
  capacity, allocation strategy, and the owner ArrayData (weird;
  required by tvDupFlattenVars).
  */
  SimpleArrayStore(const SimpleArrayStore& rhs, uint length, uint capacity,
                   ArrayData::AllocationMode am, const ArrayData* owner);
  ~SimpleArrayStore() {
    /* If this fails, it means someone didn't call destroy. */
    assert(!m_keys && !m_vals);
  }

  /**
  Destroys and deallocates all data, assumed to be of size
  length. This is effectively the destructor, implemented as a
  separate function in order to obtain length and nonSmart
  information.
  */
  void destroy(uint length, ArrayData::AllocationMode);

  /**
  The following four methods implement bidirectional iteration for
  indexes. Client code should not assume that these indices are
  contiguous. (This particular policy implementation does use
  contiguous indices.)
  */
  PosType firstIndex(uint length) const {
    assert(length);
    return PosType(0);
  }
  PosType lastIndex(uint length) const {
    assert(length);
    return toPos<uint>(length - 1);
  }
  PosType nextIndex(PosType current, uint length) const {
    auto result = toPos<uint>(toInt<int64_t>(current) + 1);
    return toInt<uint32_t>(result) < length ? result : PosType::invalid;
  }
  PosType prevIndex(PosType current, uint /*length*/) const {
    if (current == PosType(0) || current == PosType::invalid) {
      return PosType::invalid;
    }
    return toPos<uint>(toInt<uint32_t>(current) - 1);
  }

  /**
  Primitives for the next available integral key.
   */
  void nextKeyReset() { m_nextKey = 0; }
  int64_t nextKey() const { return m_nextKey; }
  int64_t nextKeyBump() { return m_nextKey++; }
  int64_t nextKeyPop() { return m_nextKey--; }

  /**
  Prepend v to the array. An uninitialized hole is left in the first
  key slot; you MUST use setKey subsequently to initialize it.
  */
  void prepend(const Variant& v, uint length, ArrayData::AllocationMode);

  /**
  Erase at position pos from the array. Cleans up data and
  everything. Caller must update its own length etc.
  */
  void erase(PosType pos, uint length);

  /**
  Construct an array store with the given allocation strategy and
  capacity.
  */
  SimpleArrayStore(ArrayData::AllocationMode am, uint capacity)
      : m_capacity(std::max<uint>(startingCapacity, capacity))
      , m_nextKey(0) {
    allocate(m_keys, m_vals, m_capacity, am);
  }

  /**
  Returns the maximum number of elements this store can hold without a
  reallocation.
  */
  uint capacity() const { return m_capacity; }

  /**
  Grow the store starting from size up to at least minSize, ideally to
  idealSize, using the given allocation strategy. Caller ensures size
  <= minSize && minSize = idealSize. No actual objects are allocated
  in the grown store.
   */
  void grow(uint size, uint minSize, uint idealSize, ArrayData::AllocationMode);

  /**
  Returns true iff the key at pos is a string.
   */
  bool hasStrKey(PosType pos) const {
    assert(m_vals && toInt<uint32_t>(pos) < m_capacity);
    return m_vals[toInt<uint32_t>(pos)].hash() != 0;
  }

  /**
  Returns the key at position pos. The result is either KindOfInt64 or
  KindOfString.
   */
  Variant key(PosType pos) const {
    assert(m_keys && toInt<uint32_t>(pos) < m_capacity);
    return hasStrKey(pos)
      ? Variant(m_keys[toInt<uint32_t>(pos)].s)
      : Variant(m_keys[toInt<uint32_t>(pos)].i);
  }

  /**
  Sets key at pos to integer k. Does NOT destroy the previous key,
  user must ensure that the previous key is released appropriately (if
  it was a string).
   */
  void setKey(PosType pos, int64_t k) {
    assert(m_keys && m_vals && toInt<uint32_t>(pos) < m_capacity);
    m_keys[toInt<uint32_t>(pos)].i = k;
    m_vals[toInt<uint32_t>(pos)].hash() = 0;
    if (m_nextKey <= k) m_nextKey = k + 1;
  }

  /**
  Sets key at pos to StringData* k. Does NOT destroy the previous key,
  user must ensure that the previous key is released appropriately (if
  it was a string). However, it DOES increment the reference count of
  the StringData.
   */
  void setKey(PosType pos, StringData* k) {
    assert(k && m_keys && m_vals && toInt<uint32_t>(pos) < m_capacity);
    m_keys[toInt<uint32_t>(pos)].s = k;
    m_vals[toInt<uint32_t>(pos)].hash() = 1;
    k->incRefCount();
  }

  /**
  Finds the given key and returns its position. If key not found,
  returns PosType::invalid.
   */
  PosType find(int64_t key, uint length) const;
  PosType find(const StringData* key, uint length) const;

  /**
  Updates the slot at position key with value. If slot did not exist,
  insert it. If slot did not exist and key is a StringData, increments
  its refcount.
   */
  template <class K>
  bool update(K key, const Variant& val, uint length,
              ArrayData::AllocationMode);

  /**
  Returns a const reference to the Variant held at position
  pos. Caller must ensure that pos is valid (i.e. corresponds to an
  existing element). Other implementations should be able to return an
  rvalue Variant, meaning they can use alternate storage strategies
  for the data.
   */
  const Variant& val(PosType pos) const {
    assert(m_vals && toInt<uint32_t>(pos) < m_capacity);
    return tvAsCVarRef(m_vals + toInt<uint32_t>(pos));
  }

  /**
  Returns a modifiable lvalue at position pos. Using the API forces
  storing values as Variant/TypedValue. Calling lval for a
  not-yet-initialized position is fine and may be used by the caller
  to initialize the value.
   */
  Variant& lval(PosType pos) const {
    assert(m_vals && toInt<uint32_t>(pos) < m_capacity);
    // Don't use tvAsVariant here (which may assert on you), the
    // reference returned may refer to a not-yet-initialized object.
    return tvAsUninitializedVariant(m_vals + toInt<uint32_t>(pos));
  }

private:
  /*****************************************************************************
  State begins here
  *****************************************************************************/
  TypedValueAux* m_vals;
  Key* m_keys;
  uint m_capacity;
  int64_t m_nextKey;
  /*****************************************************************************
  State ends here
  *****************************************************************************/
};

////////////////////////////////////////////////////////////////////////////////
// PolicyArray
////////////////////////////////////////////////////////////////////////////////
class PolicyArray : public ArrayData, private SimpleArrayStore {
  typedef SimpleArrayStore Store;
  using Store::key;

  PolicyArray(const PolicyArray& rhs) = delete;
  PolicyArray& operator=(const PolicyArray& rhs) = delete;
  PolicyArray(const PolicyArray& rhs, uint capacity, ArrayData::AllocationMode);

  template <class K>
  void addValWithRef(K k, const Variant& value);
  void nextInsertWithRef(const Variant& value);

  // Copy to a brand new HphpArray
  HphpArray* toHphpArray() const;

  // Safe downcast helpers
  static PolicyArray* asPolicyArray(ArrayData* ad);
  static const PolicyArray* asPolicyArray(const ArrayData* ad);

public:
  // Memory allocator methods.
  DECLARE_SMART_ALLOCATION(PolicyArray);
  static void Release(ArrayData*);

  explicit PolicyArray(uint size);

  virtual ~PolicyArray() FOLLY_OVERRIDE;

  /**
   * Number of elements this array has.
   */
  virtual ssize_t vsize() const FOLLY_OVERRIDE {
    // vsize() called, but m_size should never be -1 in PolicyArray
    assert(false);
    return m_size;
  }

  /**
   * getValueRef() gets a reference to value at position "pos".
   */
  virtual CVarRef getValueRef(ssize_t pos) const FOLLY_OVERRIDE;

  /*
   * Returns whether or not this array contains "vector-like" data.
   * I.e. all the keys are contiguous increasing integers.
   */
  virtual bool isVectorData() const FOLLY_OVERRIDE;

  /**
   * Testing whether a key exists.
   */
  virtual bool exists(int64_t k) const FOLLY_OVERRIDE {
    return Store::find(k, m_size) < toPos(m_size);
  }
  virtual bool exists(const StringData* k) const FOLLY_OVERRIDE {
    return Store::find(k, m_size) < toPos(m_size);
  }

private:
  template <class K> TypedValue* nvGetImpl(K k) const;

public:
  /**
   * Interface for VM helpers.  ArrayData implements generic versions
   * using the other ArrayData api; subclasses may do better.
   */
  static TypedValue* NvGetInt(const ArrayData*, int64_t k);
  static TypedValue* NvGetStr(const ArrayData*, const StringData* k);
  static void NvGetKey(const ArrayData*, TypedValue* out, ssize_t pos);

private:
  template <class K>
  ArrayData *lvalImpl(K k, Variant *&ret, bool copy);

  using Store::lval;

public:
  /**
   * Getting l-value (that Variant pointer) at specified key. Return NULL if
   * escalation is not needed, or an escalated array data.
   */
  virtual ArrayData *lval(int64_t k,
                          Variant *&ret,
                          bool copy) FOLLY_OVERRIDE {
    return lvalImpl(k, ret, copy);
  }
  virtual ArrayData *lval(StringData* k,
                          Variant*& ret,
                          bool copy) FOLLY_OVERRIDE {
    return lvalImpl(k, ret, copy);
  }

  /**
   * Getting l-value (that Variant pointer) of a new element with the next
   * available integer key. Return NULL if escalation is not needed, or an
   * escalated array data. Note that adding a new element with the next
   * available integer key may fail, in which case ret is set to point to
   * the lval blackhole (see Variant::lvalBlackHole() for details).
   */
  virtual ArrayData *lvalNew(Variant *&ret, bool copy) FOLLY_OVERRIDE;

  /**
   * Helper functions used for getting a reference to elements of
   * the dynamic property array in ObjectData or the local cache array
   * in ShardMap.
   */
  virtual ArrayData *createLvalPtr(StringData* k, Variant *&ret, bool copy)
    FOLLY_OVERRIDE;
  virtual ArrayData *getLvalPtr(StringData* k, Variant *&ret, bool copy)
    FOLLY_OVERRIDE;

  /**
   * Setting a value at specified key. If "copy" is true, make a copy first
   * then set the value. Return NULL if escalation is not needed, or an
   * escalated array data.
   */
private:
  template <class K>
  PolicyArray* setImpl(K k, CVarRef v, bool copy);

public:
  static ArrayData* SetInt(ArrayData*, int64_t k, CVarRef v, bool copy);
  static ArrayData* SetStr(ArrayData*, StringData* k, CVarRef v, bool copy);

private:
  template <class K>
  ArrayData *setRefImpl(K k, CVarRef v, bool copy);

public:
  virtual ArrayData *setRef(int64_t k, CVarRef v, bool copy) FOLLY_OVERRIDE {
    return setRefImpl(k, v, copy);
  }
  virtual ArrayData *setRef(StringData* k, CVarRef v, bool cpy) FOLLY_OVERRIDE {
    return setRefImpl(k, v, cpy);
  }

  /**
   * The same as set(), but with the precondition that the key does
   * not already exist in this array.  (This is to allow more
   * efficient implementation of this case in some derived classes.)
   */
private:
  template <class K>
  ArrayData *addImpl(K k, CVarRef v, bool copy);

public:
  virtual ArrayData *add(int64_t k, CVarRef v, bool copy) FOLLY_OVERRIDE {
    return addImpl(k, v, copy);
  }
  virtual ArrayData *add(StringData* k, CVarRef v, bool copy) FOLLY_OVERRIDE {
    return addImpl(k, v, copy);
  }

  /*
   * Same semantics as lval(), except with the precondition that the
   * key doesn't already exist in the array.
   */
private:
  template <class K>
  PolicyArray* addLvalImpl(K k, Variant*& ret, bool copy);

public:
  virtual PolicyArray *addLval(int64_t k, Variant *&ret, bool copy)
    FOLLY_OVERRIDE {
    return addLvalImpl(k, ret, copy);
  }
  virtual PolicyArray *addLval(StringData* k, Variant *&ret, bool copy)
    FOLLY_OVERRIDE {
    return addLvalImpl(k, ret, copy);
  }

  /**
   * Remove a value at specified key. If "copy" is true, make a copy first
   * then remove the value. Return NULL if escalation is not needed, or an
   * escalated array data.
   */
private:
  template <class K> ArrayData *removeImpl(K k, bool copy);

public:
  virtual ArrayData *remove(int64_t k, bool copy) FOLLY_OVERRIDE {
    return removeImpl(k, copy);
  }
  virtual ArrayData *remove(const StringData* k, bool copy) FOLLY_OVERRIDE {
    return removeImpl(k, copy);
  }

  virtual ssize_t iter_begin() const FOLLY_OVERRIDE;
  virtual ssize_t iter_end() const FOLLY_OVERRIDE;
  virtual ssize_t iter_advance(ssize_t prev) const FOLLY_OVERRIDE;
  virtual ssize_t iter_rewind(ssize_t prev) const FOLLY_OVERRIDE;

  /**
   * Mutable iteration APIs
   *
   * The following six methods are used for mutable iteration. For all methods
   * except newFullPos(), it is the caller's responsibility to ensure that the
   * specified FullPos 'fp' is registered with this array and hasn't already
   * been freed.
   */

  /**
   * Checks if a mutable iterator points to a valid element within this array.
   * This will return false if the iterator points past the last element, or
   * if the iterator points before the first element.
   */
  virtual bool validFullPos(const FullPos& fp) const FOLLY_OVERRIDE;

  /**
   * Advances the mutable iterator to the next element in the array. Returns
   * false if the iterator has moved past the last element, otherwise returns
   * true.
   */
  virtual bool advanceFullPos(FullPos& fp) FOLLY_OVERRIDE;

  virtual ArrayData* escalateForSort() FOLLY_OVERRIDE;

  /**
   * Make a copy of myself.
   *
   * The nonSmartCopy() version means not to use the smart allocator.
   * Is only implemented for array types that need to be able to go
   * into the static array list.
   */
  PolicyArray* copy(uint minCapacity);
  virtual PolicyArray *copy() const FOLLY_OVERRIDE;
  virtual PolicyArray *copyWithStrongIterators() const FOLLY_OVERRIDE;
  virtual ArrayData *nonSmartCopy() const FOLLY_OVERRIDE;

private:
  template <class K, class V>
  Variant* appendNoGrow(K k, const V& v) {
    assert(m_size < Store::capacity());
    auto result = &lval(toPos(m_size));
    // WARNING: sequencing the following two lines is important.
    new(result) Variant(v);
    setKey(toPos(m_size), k);
    // If position is at end, make it point to the element just added.
    if (size_t(m_pos) > m_size) m_pos = m_size;
    ++m_size;
    return result;
  }

public:

  /**
   * Append a value to the array. If "copy" is true, make a copy first
   * then append the value. Return NULL if escalation is not needed, or an
   * escalated array data.
   */
  static ArrayData* Append(ArrayData*, CVarRef v, bool copy);
  virtual PolicyArray *appendRef(CVarRef v, bool copy) FOLLY_OVERRIDE;

  /**
   * Similar to append(v, copy), with reference in v preserved.
   */
  virtual ArrayData *appendWithRef(CVarRef v, bool copy) FOLLY_OVERRIDE;

  /**
   * Implementing array appending and merging. If "copy" is true, make a copy
   * first then append/merge arrays. Return NULL if escalation is not needed,
   * or an escalated array data.
   */
  virtual ArrayData *plus(const ArrayData *elems, bool copy) FOLLY_OVERRIDE;
  virtual ArrayData *merge(const ArrayData *elems, bool copy) FOLLY_OVERRIDE;

  /**
   * Stack function: pop the last item and return it.
   */
  virtual ArrayData *pop(Variant &value) FOLLY_OVERRIDE;

  /**
   * Queue function: remove the 1st item and return it.
   */
  virtual ArrayData *dequeue(Variant &value) FOLLY_OVERRIDE;

  /**
   * Array function: prepend a new item.
   */
  virtual ArrayData *prepend(CVarRef v, bool copy) FOLLY_OVERRIDE;

  /**
   * Only map classes need this. Re-index all numeric keys to start from 0.
   */
  virtual void renumber() FOLLY_OVERRIDE;

  virtual void onSetEvalScalar() FOLLY_OVERRIDE;

  virtual ArrayData *escalate() const FOLLY_OVERRIDE;
};

///////////////////////////////////////////////////////////////////////////////
}

#endif // HPHP_POLICY_ARRAY_H_
