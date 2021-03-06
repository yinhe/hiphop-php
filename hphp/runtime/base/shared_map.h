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

#ifndef incl_HPHP_SHARED_MAP_H_
#define incl_HPHP_SHARED_MAP_H_

#include "hphp/util/shared_memory_allocator.h"
#include "hphp/runtime/base/shared_variant.h"
#include "hphp/runtime/base/array_data.h"
#include "hphp/runtime/base/complex_types.h"
#include "hphp/runtime/base/builtin_functions.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/**
 * Wrapper for a shared memory map.
 */
class SharedMap : public ArrayData {
public:
  explicit SharedMap(SharedVariant* source)
      : ArrayData(kSharedKind)
      , m_localCache(nullptr) {
    m_map      = source->getMap();
    m_isVector = source->getIsVector();
    m_size = isVector() ? m_vec->m_size : m_map->size();
  }

  ~SharedMap();

  virtual bool isSharedMap() const { return true; }

  // these using directives ensure the full set of overloaded functions
  // are visible in this class, to avoid triggering implicit conversions
  // from a CVarRef key to int64.
  using ArrayData::exists;
  using ArrayData::lval;
  using ArrayData::lvalNew;
  using ArrayData::set;
  using ArrayData::setRef;
  using ArrayData::add;
  using ArrayData::addLval;
  using ArrayData::remove;

  ssize_t vsize() const;

  SharedVariant* getValueImpl(ssize_t pos) const {
    return isVector() ? m_vec->getValue(pos) : m_map->getValue(pos);
  }

  CVarRef getValueRef(ssize_t pos) const;

  bool exists(int64_t k) const;
  bool exists(const StringData* k) const;

  virtual ArrayData *lval(int64_t k, Variant *&ret, bool copy);
  virtual ArrayData *lval(StringData* k, Variant *&ret, bool copy);
  ArrayData *lvalNew(Variant *&ret, bool copy);

  static ArrayData *SetInt(ArrayData*, int64_t k, CVarRef v, bool copy);
  static ArrayData *SetStr(ArrayData*, StringData* k, CVarRef v, bool copy);
  ArrayData *setRef(int64_t k, CVarRef v, bool copy);
  ArrayData *setRef(StringData* k, CVarRef v, bool copy);

  ArrayData *remove(int64_t k, bool copy);
  ArrayData *remove(const StringData* k, bool copy);

  ArrayData *copy() const;
  /**
   * Copy (escalate) the SharedMap without triggering local cache.
   */
  static ArrayData *Append(ArrayData* a, CVarRef v, bool copy);
  ArrayData *appendRef(CVarRef v, bool copy);
  ArrayData *appendWithRef(CVarRef v, bool copy);
  ArrayData *plus(const ArrayData *elems, bool copy);
  ArrayData *merge(const ArrayData *elems, bool copy);

  ArrayData *prepend(CVarRef v, bool copy);

  /**
   * Non-Variant virtual methods that override ArrayData
   */
  static TypedValue* NvGetInt(const ArrayData*, int64_t k);
  static TypedValue* NvGetStr(const ArrayData*, const StringData* k);
  static void NvGetKey(const ArrayData*, TypedValue* out, ssize_t pos);

  bool isVectorData() const;

  ssize_t iter_begin() const;
  ssize_t iter_end() const;
  ssize_t iter_advance(ssize_t prev) const;
  ssize_t iter_rewind(ssize_t prev) const;

  bool validFullPos(const FullPos& fp) const;
  bool advanceFullPos(FullPos& fp);

  /**
   * Memory allocator methods.
   */
  DECLARE_SMART_ALLOCATION(SharedMap);
  static void Release(ArrayData*);

  virtual ArrayData *escalate() const;
  virtual ArrayData* escalateForSort();

  ArrayData* loadElems(bool mapInit = false) const;

private:
  ssize_t getIndex(int64_t k) const;
  ssize_t getIndex(const StringData* k) const;
  static SharedMap* asSharedMap(ArrayData* ad);
  static const SharedMap* asSharedMap(const ArrayData* ad);

private:
  bool m_isVector;
  union {
    ImmutableMap* m_map;
    VectorData*   m_vec;
  };
  mutable TypedValue* m_localCache;
  bool isVector() const { return m_isVector; }

public:
  void getChildren(std::vector<TypedValue *> &out);
};

///////////////////////////////////////////////////////////////////////////////
}

#endif // incl_HPHP_SHARED_MAP_H_
