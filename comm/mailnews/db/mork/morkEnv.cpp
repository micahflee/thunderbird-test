/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-  */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _MDB_
#  include "mdb.h"
#endif

#ifndef _MORK_
#  include "mork.h"
#endif

#ifndef _MORKNODE_
#  include "morkNode.h"
#endif

#ifndef _MORKCH_
#  include "morkCh.h"
#endif

#ifndef _MORKENV_
#  include "morkEnv.h"
#endif

#ifndef _MORKFACTORY_
#  include "morkFactory.h"
#endif

#include "mozilla/Char16.h"

// 456789_123456789_123456789_123456789_123456789_123456789_123456789_123456789

// ````` ````` ````` ````` `````
// { ===== begin morkNode interface =====

/*public virtual*/ void morkEnv::CloseMorkNode(
    morkEnv* ev) /*i*/  // CloseEnv() only if open
{
  if (this->IsOpenNode()) {
    this->MarkClosing();
    this->CloseEnv(ev);
    this->MarkShut();
  }
}

/*public virtual*/
morkEnv::~morkEnv() /*i*/  // assert CloseEnv() executed earlier
{
  CloseMorkNode(mMorkEnv);
  if (mEnv_Heap) {
    mork_bool ownsHeap = mEnv_OwnsHeap;
    nsIMdbHeap* saveHeap = mEnv_Heap;

    if (ownsHeap) {
#ifdef MORK_DEBUG_HEAP_STATS
      printf("%d blocks remaining \n",
             ((orkinHeap*)saveHeap)->HeapBlockCount());
      mork_u4* array = (mork_u4*)this;
      array -= 3;
      // null out heap ptr in mem block so we won't crash trying to use it to
      // delete the env.
      *array = nullptr;
#endif  // MORK_DEBUG_HEAP_STATS
      // whoops, this is our heap - hmm. Can't delete it, or not allocate env's
      // from an orkinHeap.
      delete saveHeap;
    }
  }
  //  MORK_ASSERT(mEnv_SelfAsMdbEnv==0);
  MORK_ASSERT(mEnv_ErrorHook == 0);
}

/* choose morkBool_kTrue or morkBool_kFalse for kBeVerbose: */
#define morkEnv_kBeVerbose morkBool_kFalse

/*public non-poly*/
morkEnv::morkEnv(const morkUsage& inUsage, nsIMdbHeap* ioHeap,
                 morkFactory* ioFactory, nsIMdbHeap* ioSlotHeap)
    : morkObject(inUsage, ioHeap, morkColor_kNone),
      mEnv_Factory(ioFactory),
      mEnv_Heap(ioSlotHeap)

      ,
      mEnv_SelfAsMdbEnv(0),
      mEnv_ErrorHook(0),
      mEnv_HandlePool(0)

      ,
      mEnv_ErrorCount(0),
      mEnv_WarningCount(0)

      ,
      mEnv_ErrorCode(NS_OK)

      ,
      mEnv_DoTrace(morkBool_kFalse),
      mEnv_AutoClear(morkAble_kDisabled),
      mEnv_ShouldAbort(morkBool_kFalse),
      mEnv_BeVerbose(morkEnv_kBeVerbose),
      mEnv_OwnsHeap(morkBool_kFalse) {
  MORK_ASSERT(ioSlotHeap && ioFactory);
  if (ioSlotHeap) {
    // mEnv_Heap is NOT refcounted:
    // nsIMdbHeap_SlotStrongHeap(ioSlotHeap, this, &mEnv_Heap);

    mEnv_HandlePool =
        new morkPool(morkUsage::kGlobal, (nsIMdbHeap*)0, ioSlotHeap);

    MORK_ASSERT(mEnv_HandlePool);
    if (mEnv_HandlePool && this->Good()) {
      mNode_Derived = morkDerived_kEnv;
      mNode_Refs += morkEnv_kWeakRefCountEnvBonus;
    }
  }
}

/*public non-poly*/
morkEnv::morkEnv(morkEnv* ev, /*i*/
                 const morkUsage& inUsage, nsIMdbHeap* ioHeap,
                 nsIMdbEnv* inSelfAsMdbEnv, morkFactory* ioFactory,
                 nsIMdbHeap* ioSlotHeap)
    : morkObject(ev, inUsage, ioHeap, morkColor_kNone, (morkHandle*)0),
      mEnv_Factory(ioFactory),
      mEnv_Heap(ioSlotHeap)

      ,
      mEnv_SelfAsMdbEnv(inSelfAsMdbEnv),
      mEnv_ErrorHook(0),
      mEnv_HandlePool(0)

      ,
      mEnv_ErrorCount(0),
      mEnv_WarningCount(0)

      ,
      mEnv_ErrorCode(NS_OK)

      ,
      mEnv_DoTrace(morkBool_kFalse),
      mEnv_AutoClear(morkAble_kDisabled),
      mEnv_ShouldAbort(morkBool_kFalse),
      mEnv_BeVerbose(morkEnv_kBeVerbose),
      mEnv_OwnsHeap(morkBool_kFalse) {
  // $$$ do we need to refcount the inSelfAsMdbEnv nsIMdbEnv??

  if (ioFactory && inSelfAsMdbEnv && ioSlotHeap) {
    // mEnv_Heap is NOT refcounted:
    // nsIMdbHeap_SlotStrongHeap(ioSlotHeap, ev, &mEnv_Heap);

    mEnv_HandlePool = new (*ioSlotHeap, ev)
        morkPool(ev, morkUsage::kHeap, ioSlotHeap, ioSlotHeap);

    MORK_ASSERT(mEnv_HandlePool);
    if (mEnv_HandlePool && ev->Good()) {
      mNode_Derived = morkDerived_kEnv;
      mNode_Refs += morkEnv_kWeakRefCountEnvBonus;
    }
  } else
    ev->NilPointerError();
}

NS_IMPL_ISUPPORTS_INHERITED(morkEnv, morkObject, nsIMdbEnv)
/*public non-poly*/ void morkEnv::CloseEnv(
    morkEnv* ev) /*i*/  // called by CloseMorkNode();
{
  if (this->IsNode()) {
    // $$$ release mEnv_SelfAsMdbEnv??
    // $$$ release mEnv_ErrorHook??

    mEnv_SelfAsMdbEnv = 0;
    mEnv_ErrorHook = 0;

    morkPool* savePool = mEnv_HandlePool;
    morkPool::SlotStrongPool((morkPool*)0, ev, &mEnv_HandlePool);
    // free the pool
    if (mEnv_SelfAsMdbEnv) {
      if (savePool && mEnv_Heap) mEnv_Heap->Free(this->AsMdbEnv(), savePool);
    } else {
      if (savePool) {
        if (savePool->IsOpenNode()) savePool->CloseMorkNode(ev);
        delete savePool;
      }
      // how do we free this? might need to get rid of asserts.
    }
    // mEnv_Factory is NOT refcounted

    this->MarkShut();
  } else
    this->NonNodeError(ev);
}

// } ===== end morkNode methods =====
// ````` ````` ````` ````` `````

mork_size morkEnv::OidAsHex(void* outBuf, const mdbOid& inOid)
// sprintf(buf, "%lX:^%lX", (long) inOid.mOid_Id, (long) inOid.mOid_Scope);
{
  mork_u1* p = (mork_u1*)outBuf;
  mork_size outSize = this->TokenAsHex(p, inOid.mOid_Id);
  p += outSize;
  *p++ = ':';

  mork_scope scope = inOid.mOid_Scope;
  if (scope < 0x80 && morkCh_IsName((mork_ch)scope)) {
    *p++ = (mork_u1)scope;
    *p = 0;  // null termination
    outSize += 2;
  } else {
    *p++ = '^';
    mork_size scopeSize = this->TokenAsHex(p, scope);
    outSize += scopeSize + 2;
  }
  return outSize;
}

mork_u1 morkEnv::HexToByte(mork_ch inFirstHex, mork_ch inSecondHex) {
  mork_u1 hi = 0;  // high four hex bits
  mork_flags f = morkCh_GetFlags(inFirstHex);
  if (morkFlags_IsDigit(f))
    hi = (mork_u1)(inFirstHex - (mork_ch)'0');
  else if (morkFlags_IsUpper(f))
    hi = (mork_u1)((inFirstHex - (mork_ch)'A') + 10);
  else if (morkFlags_IsLower(f))
    hi = (mork_u1)((inFirstHex - (mork_ch)'a') + 10);

  mork_u1 lo = 0;  // low four hex bits
  f = morkCh_GetFlags(inSecondHex);
  if (morkFlags_IsDigit(f))
    lo = (mork_u1)(inSecondHex - (mork_ch)'0');
  else if (morkFlags_IsUpper(f))
    lo = (mork_u1)((inSecondHex - (mork_ch)'A') + 10);
  else if (morkFlags_IsLower(f))
    lo = (mork_u1)((inSecondHex - (mork_ch)'a') + 10);

  return (mork_u1)((hi << 4) | lo);
}

// TokenAsHex() is the same as sprintf(outBuf, "%lX", (long) inToken);
// Writes up to 32 hex digits, plus a NUL-terminator. So outBuf must
// be at least 33 bytes.
// Return value is number of characters written, excluding the NUL.
mork_size morkEnv::TokenAsHex(void* outBuf, mork_token inToken) {
  static const char morkEnv_kHexDigits[] = "0123456789ABCDEF";
  char* p = (char*)outBuf;
  char* end = p + 32;  // write no more than 32 digits for safety
  if (inToken) {
    // first write all the hex digits in backwards order:
    while (p < end && inToken)  // more digits to write?
    {
      *p++ = morkEnv_kHexDigits[inToken & 0x0F];  // low four bits
      inToken >>= 4;  // we fervently hope this does not sign extend
    }
    *p = 0;                               // end the string with a null byte
    char* s = (char*)outBuf;              // first byte in string
    mork_size size = (mork_size)(p - s);  // distance from start

    // now reverse the string in place:
    // note that p starts on the null byte, so we need predecrement:
    while (--p > s)  // need to swap another byte in the string?
    {
      char c = *p;  // temp for swap
      *p = *s;
      *s++ = c;  // move s forward here, and p backward in the test
    }
    return size;
  } else  // special case for zero integer
  {
    *p++ = '0';  // write a zero digit
    *p = 0;      // end with a null byte
    return 1;    // one digit in hex representation
  }
}

void morkEnv::StringToYarn(const PathChar* inString, mdbYarn* outYarn) {
  if (outYarn) {
    mdb_fill fill =
        (inString) ? (mdb_fill)MORK_STRLEN(inString) * sizeof(PathChar) : 0;

    if (fill)  // have nonempty content?
    {
      mdb_size size = outYarn->mYarn_Size;  // max dest size
      if (fill > size)                      // too much string content?
      {
        outYarn->mYarn_More = fill - size;  // extra string bytes omitted
        fill = size;  // copy no more bytes than size of yarn buffer
      }
      void* dest = outYarn->mYarn_Buf;  // where bytes are going
      if (!dest)                        // nil destination address buffer?
        fill = 0;                       // we can't write any content at all

      if (fill)                             // anything to copy?
        MORK_MEMCPY(dest, inString, fill);  // copy fill bytes to yarn

      outYarn->mYarn_Fill = fill;  // tell yarn size of copied content
    } else                         // no content to put into the yarn
    {
      outYarn->mYarn_Fill = 0;  // tell yarn that string has no bytes
    }
    outYarn->mYarn_Form = 0;  // always update the form slot
  } else
    this->NilPointerError();
}

morkEnv::PathChar* morkEnv::CopyString(nsIMdbHeap* ioHeap,
                                       const PathChar* inString) {
  PathChar* outString = nullptr;
  if (ioHeap && inString) {
    mork_size size = (MORK_STRLEN(inString) + 1) * sizeof(PathChar);
    ioHeap->Alloc(this->AsMdbEnv(), size, (void**)&outString);
    if (outString) MORK_STRCPY(outString, inString);
  } else
    this->NilPointerError();
  return outString;
}

void morkEnv::FreeString(nsIMdbHeap* ioHeap, PathChar* ioString) {
  if (ioHeap) {
    if (ioString) ioHeap->Free(this->AsMdbEnv(), ioString);
  } else
    this->NilPointerError();
}

void morkEnv::NewError(const char* inString) {
  MORK_ASSERT(morkBool_kFalse);  // get developer's attention

  ++mEnv_ErrorCount;
  mEnv_ErrorCode = NS_ERROR_FAILURE;

  if (mEnv_ErrorHook) mEnv_ErrorHook->OnErrorString(this->AsMdbEnv(), inString);
}

void morkEnv::NewWarning(const char* inString) {
  MORK_ASSERT(morkBool_kFalse);  // get developer's attention

  ++mEnv_WarningCount;
  if (mEnv_ErrorHook)
    mEnv_ErrorHook->OnWarningString(this->AsMdbEnv(), inString);
}

void morkEnv::StubMethodOnlyError() { this->NewError("method is stub only"); }

void morkEnv::OutOfMemoryError() { this->NewError("out of memory"); }

void morkEnv::CantMakeWhenBadError() {
  this->NewError("can't make an object when ev->Bad()");
}

static const char morkEnv_kNilPointer[] = "nil pointer";

void morkEnv::NilPointerError() { this->NewError(morkEnv_kNilPointer); }

void morkEnv::NilPointerWarning() { this->NewWarning(morkEnv_kNilPointer); }

void morkEnv::NewNonEnvError() { this->NewError("non-env instance"); }

void morkEnv::NilEnvSlotError() {
  if (!mEnv_HandlePool || !mEnv_Factory) {
    if (!mEnv_HandlePool) this->NewError("nil mEnv_HandlePool");
    if (!mEnv_Factory) this->NewError("nil mEnv_Factory");
  } else
    this->NewError("unknown nil env slot");
}

void morkEnv::NonEnvTypeError(morkEnv* ev) { ev->NewError("non morkEnv"); }

void morkEnv::ClearMorkErrorsAndWarnings() {
  mEnv_ErrorCount = 0;
  mEnv_WarningCount = 0;
  mEnv_ErrorCode = NS_OK;
  mEnv_ShouldAbort = morkBool_kFalse;
}

void morkEnv::AutoClearMorkErrorsAndWarnings() {
  if (this->DoAutoClear()) {
    mEnv_ErrorCount = 0;
    mEnv_WarningCount = 0;
    mEnv_ErrorCode = NS_OK;
    mEnv_ShouldAbort = morkBool_kFalse;
  }
}

/*static*/ morkEnv* morkEnv::FromMdbEnv(
    nsIMdbEnv* ioEnv)  // dynamic type checking
{
  morkEnv* outEnv = 0;
  if (ioEnv) {
    // Note this cast is expected to perform some address adjustment of the
    // pointer, so oenv likely does not equal ioEnv.  Do not cast to void*
    // first to force an exactly equal pointer (we tried it and it's wrong).
    morkEnv* ev = (morkEnv*)ioEnv;
    if (ev && ev->IsEnv()) {
      if (ev->DoAutoClear()) {
        ev->mEnv_ErrorCount = 0;
        ev->mEnv_WarningCount = 0;
        ev->mEnv_ErrorCode = NS_OK;
      }
      outEnv = ev;
    } else
      MORK_ASSERT(outEnv);
  } else
    MORK_ASSERT(outEnv);
  return outEnv;
}

NS_IMETHODIMP
morkEnv::GetErrorCount(mdb_count* outCount, mdb_bool* outShouldAbort) {
  if (outCount) *outCount = mEnv_ErrorCount;
  if (outShouldAbort) *outShouldAbort = mEnv_ShouldAbort;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::GetWarningCount(mdb_count* outCount, mdb_bool* outShouldAbort) {
  if (outCount) *outCount = mEnv_WarningCount;
  if (outShouldAbort) *outShouldAbort = mEnv_ShouldAbort;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::GetEnvBeVerbose(mdb_bool* outBeVerbose) {
  NS_ENSURE_ARG_POINTER(outBeVerbose);
  *outBeVerbose = mEnv_BeVerbose;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::SetEnvBeVerbose(mdb_bool inBeVerbose) {
  mEnv_BeVerbose = inBeVerbose;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::GetDoTrace(mdb_bool* outDoTrace) {
  NS_ENSURE_ARG_POINTER(outDoTrace);
  *outDoTrace = mEnv_DoTrace;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::SetDoTrace(mdb_bool inDoTrace) {
  mEnv_DoTrace = inDoTrace;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::GetAutoClear(mdb_bool* outAutoClear) {
  NS_ENSURE_ARG_POINTER(outAutoClear);
  *outAutoClear = DoAutoClear();
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::SetAutoClear(mdb_bool inAutoClear) {
  if (inAutoClear)
    EnableAutoClear();
  else
    DisableAutoClear();
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::GetErrorHook(nsIMdbErrorHook** acqErrorHook) {
  NS_ENSURE_ARG_POINTER(acqErrorHook);
  *acqErrorHook = mEnv_ErrorHook;
  NS_IF_ADDREF(mEnv_ErrorHook);
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::SetErrorHook(nsIMdbErrorHook* ioErrorHook)  // becomes referenced
{
  mEnv_ErrorHook = ioErrorHook;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::GetHeap(nsIMdbHeap** acqHeap) {
  NS_ENSURE_ARG_POINTER(acqHeap);
  nsIMdbHeap* outHeap = mEnv_Heap;

  if (acqHeap) *acqHeap = outHeap;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::SetHeap(nsIMdbHeap* ioHeap)  // becomes referenced
{
  nsIMdbHeap_SlotStrongHeap(ioHeap, this, &mEnv_Heap);
  return NS_OK;
}
// } ----- end attribute methods -----

NS_IMETHODIMP
morkEnv::ClearErrors()  // clear errors beore re-entering db API
{
  mEnv_ErrorCount = 0;
  mEnv_ErrorCode = NS_OK;
  mEnv_ShouldAbort = morkBool_kFalse;

  return NS_OK;
}

NS_IMETHODIMP
morkEnv::ClearWarnings()  // clear warning
{
  mEnv_WarningCount = 0;
  return NS_OK;
}

NS_IMETHODIMP
morkEnv::ClearErrorsAndWarnings()  // clear both errors & warnings
{
  ClearMorkErrorsAndWarnings();
  return NS_OK;
}
// } ===== end nsIMdbEnv methods =====

// 456789_123456789_123456789_123456789_123456789_123456789_123456789_123456789
