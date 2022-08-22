/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * A SurfacePipe is a pipeline that consists of a series of SurfaceFilters
 * terminating in a SurfaceSink. Each SurfaceFilter transforms the image data in
 * some way before the SurfaceSink ultimately writes it to the surface. This
 * design allows for each transformation to be tested independently, for the
 * transformations to be combined as needed to meet the needs of different
 * situations, and for all image decoders to share the same code for these
 * transformations.
 *
 * Writing to the SurfacePipe is done using lambdas that act as generator
 * functions. Because the SurfacePipe machinery controls where the writes take
 * place, a bug in an image decoder cannot cause a buffer overflow of the
 * underlying surface.
 */

#ifndef mozilla_image_SurfacePipe_h
#define mozilla_image_SurfacePipe_h

#include <stdint.h>

#include <utility>

#include "AnimationParams.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/Tuple.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Unused.h"
#include "mozilla/Variant.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Swizzle.h"
#include "nsDebug.h"
#include "Orientation.h"

namespace mozilla {
namespace image {

class Decoder;

/**
 * An invalid rect for a surface. Results are given both in the space of the
 * input image (i.e., before any SurfaceFilters are applied) and in the space
 * of the output surface (after all SurfaceFilters).
 */
struct SurfaceInvalidRect {
  OrientedIntRect
      mInputSpaceRect;  /// The invalid rect in pre-SurfacePipe space.
  OrientedIntRect
      mOutputSpaceRect;  /// The invalid rect in post-SurfacePipe space.
};

/**
 * An enum used to allow the lambdas passed to WritePixels() to communicate
 * their state to the caller.
 */
enum class WriteState : uint8_t {
  NEED_MORE_DATA,  /// The lambda ran out of data.

  FINISHED,  /// The lambda is done writing to the surface; future writes
             /// will fail.

  FAILURE  /// The lambda encountered an error. The caller may recover
           /// if possible and continue to write. (This never indicates
           /// an error in the SurfacePipe machinery itself; it's only
           /// generated by the lambdas.)
};

/**
 * A template alias used to make the return value of WritePixels() lambdas
 * (which may return either a pixel value or a WriteState) easier to specify.
 */
template <typename PixelType>
using NextPixel = Variant<PixelType, WriteState>;

/**
 * SurfaceFilter is the abstract superclass of SurfacePipe pipeline stages.  It
 * implements the the code that actually writes to the surface - WritePixels()
 * and the other Write*() methods - which are non-virtual for efficiency.
 *
 * SurfaceFilter's API is nonpublic; only SurfacePipe and other SurfaceFilters
 * should use it. Non-SurfacePipe code should use the methods on SurfacePipe.
 *
 * To implement a SurfaceFilter, it's necessary to subclass SurfaceFilter and
 * implement, at a minimum, the pure virtual methods. It's also necessary to
 * define a Config struct with a Filter typedef member that identifies the
 * matching SurfaceFilter class, and a Configure() template method. See an
 * existing SurfaceFilter subclass, such as RemoveFrameRectFilter, for an
 * example of how the Configure() method must be implemented. It takes a list of
 * Config structs, passes the tail of the list to the next filter in the chain's
 * Configure() method, and then uses the head of the list to configure itself. A
 * SurfaceFilter's Configure() method must also call
 * SurfaceFilter::ConfigureFilter() to provide the Write*() methods with the
 * information they need to do their jobs.
 */
class SurfaceFilter {
 public:
  SurfaceFilter() : mRowPointer(nullptr), mCol(0), mPixelSize(0) {}

  virtual ~SurfaceFilter() {}

  /**
   * Reset this surface to the first row. It's legal for this filter to throw
   * away any previously written data at this point, as all rows must be written
   * to on every pass.
   *
   * @return a pointer to the buffer for the first row.
   */
  uint8_t* ResetToFirstRow() {
    mCol = 0;
    mRowPointer = DoResetToFirstRow();
    return mRowPointer;
  }

  /**
   * Called by WritePixels() to advance this filter to the next row.
   *
   * @return a pointer to the buffer for the next row, or nullptr to indicate
   *         that we've finished the entire surface.
   */
  uint8_t* AdvanceRow() {
    mCol = 0;
    mRowPointer = DoAdvanceRow();
    return mRowPointer;
  }

  /**
   * Called by WriteBuffer() to advance this filter to the next row, if the
   * supplied row is a full row.
   *
   * @return a pointer to the buffer for the next row, or nullptr to indicate
   *         that we've finished the entire surface.
   */
  uint8_t* AdvanceRow(const uint8_t* aInputRow) {
    mCol = 0;
    mRowPointer = DoAdvanceRowFromBuffer(aInputRow);
    return mRowPointer;
  }

  /// @return a pointer to the buffer for the current row.
  uint8_t* CurrentRowPointer() const { return mRowPointer; }

  /// @return true if we've finished writing to the surface.
  bool IsSurfaceFinished() const { return mRowPointer == nullptr; }

  /// @return the input size this filter expects.
  gfx::IntSize InputSize() const { return mInputSize; }

  /**
   * Write pixels to the surface one at a time by repeatedly calling a lambda
   * that yields pixels. WritePixels() is completely memory safe.
   *
   * Writing continues until every pixel in the surface has been written to
   * (i.e., IsSurfaceFinished() returns true) or the lambda returns a WriteState
   * which WritePixels() will return to the caller.
   *
   * The template parameter PixelType must be uint8_t (for paletted surfaces) or
   * uint32_t (for BGRA/BGRX surfaces) and must be in agreement with the pixel
   * size passed to ConfigureFilter().
   *
   * XXX(seth): We'll remove all support for paletted surfaces in bug 1247520,
   * which means we can remove the PixelType template parameter from this
   * method.
   *
   * @param aFunc A lambda that functions as a generator, yielding the next
   *              pixel in the surface each time it's called. The lambda must
   *              return a NextPixel<PixelType> value.
   *
   * @return A WriteState value indicating the lambda generator's state.
   *         WritePixels() itself will return WriteState::FINISHED if writing
   *         has finished, regardless of the lambda's internal state.
   */
  template <typename PixelType, typename Func>
  WriteState WritePixels(Func aFunc) {
    Maybe<WriteState> result;
    while (
        !(result = DoWritePixelsToRow<PixelType>(std::forward<Func>(aFunc)))) {
    }

    return *result;
  }

  /**
   * Write pixels to the surface by calling a lambda which may write as many
   * pixels as there is remaining to complete the row. It is not completely
   * memory safe as it trusts the underlying decoder not to overrun the given
   * buffer, however it is an acceptable tradeoff for performance.
   *
   * Writing continues until every pixel in the surface has been written to
   * (i.e., IsSurfaceFinished() returns true) or the lambda returns a WriteState
   * which WritePixelBlocks() will return to the caller.
   *
   * The template parameter PixelType must be uint8_t (for paletted surfaces) or
   * uint32_t (for BGRA/BGRX surfaces) and must be in agreement with the pixel
   * size passed to ConfigureFilter().
   *
   * XXX(seth): We'll remove all support for paletted surfaces in bug 1247520,
   * which means we can remove the PixelType template parameter from this
   * method.
   *
   * @param aFunc A lambda that functions as a generator, yielding at most the
   *              maximum number of pixels requested. The lambda must accept a
   *              pointer argument to the first pixel to write, a maximum
   *              number of pixels to write as part of the block, and return a
   *              NextPixel<PixelType> value.
   *
   * @return A WriteState value indicating the lambda generator's state.
   *         WritePixelBlocks() itself will return WriteState::FINISHED if
   *         writing has finished, regardless of the lambda's internal state.
   */
  template <typename PixelType, typename Func>
  WriteState WritePixelBlocks(Func aFunc) {
    Maybe<WriteState> result;
    while (!(result = DoWritePixelBlockToRow<PixelType>(
                 std::forward<Func>(aFunc)))) {
    }

    return *result;
  }

  /**
   * A variant of WritePixels() that writes a single row of pixels to the
   * surface one at a time by repeatedly calling a lambda that yields pixels.
   * WritePixelsToRow() is completely memory safe.
   *
   * Writing continues until every pixel in the row has been written to. If the
   * surface is complete at that pointer, WriteState::FINISHED is returned;
   * otherwise, WritePixelsToRow() returns WriteState::NEED_MORE_DATA. The
   * lambda can terminate writing early by returning a WriteState itself, which
   * WritePixelsToRow() will return to the caller.
   *
   * The template parameter PixelType must be uint8_t (for paletted surfaces) or
   * uint32_t (for BGRA/BGRX surfaces) and must be in agreement with the pixel
   * size passed to ConfigureFilter().
   *
   * XXX(seth): We'll remove all support for paletted surfaces in bug 1247520,
   * which means we can remove the PixelType template parameter from this
   * method.
   *
   * @param aFunc A lambda that functions as a generator, yielding the next
   *              pixel in the surface each time it's called. The lambda must
   *              return a NextPixel<PixelType> value.
   *
   * @return A WriteState value indicating the lambda generator's state.
   *         WritePixels() itself will return WriteState::FINISHED if writing
   *         the entire surface has finished, or WriteState::NEED_MORE_DATA if
   *         writing the row has finished, regardless of the lambda's internal
   *         state.
   */
  template <typename PixelType, typename Func>
  WriteState WritePixelsToRow(Func aFunc) {
    return DoWritePixelsToRow<PixelType>(std::forward<Func>(aFunc))
        .valueOr(WriteState::NEED_MORE_DATA);
  }

  /**
   * Write a row to the surface by copying from a buffer. This is bounds checked
   * and memory safe with respect to the surface, but care must still be taken
   * by the caller not to overread the source buffer. This variant of
   * WriteBuffer() requires a source buffer which contains |mInputSize.width|
   * pixels.
   *
   * The template parameter PixelType must be uint8_t (for paletted surfaces) or
   * uint32_t (for BGRA/BGRX surfaces) and must be in agreement with the pixel
   * size passed to ConfigureFilter().
   *
   * XXX(seth): We'll remove all support for paletted surfaces in bug 1247520,
   * which means we can remove the PixelType template parameter from this
   * method.
   *
   * @param aSource A buffer to copy from. This buffer must be
   *                |mInputSize.width| pixels wide,  which means
   *                |mInputSize.width * sizeof(PixelType)| bytes. May not be
   *                null.
   *
   * @return WriteState::FINISHED if the entire surface has been written to.
   *         Otherwise, returns WriteState::NEED_MORE_DATA. If a null |aSource|
   *         value is passed, returns WriteState::FAILURE.
   */
  template <typename PixelType>
  WriteState WriteBuffer(const PixelType* aSource) {
    MOZ_ASSERT(mPixelSize == 1 || mPixelSize == 4);
    MOZ_ASSERT_IF(mPixelSize == 1, sizeof(PixelType) == sizeof(uint8_t));
    MOZ_ASSERT_IF(mPixelSize == 4, sizeof(PixelType) == sizeof(uint32_t));

    if (IsSurfaceFinished()) {
      return WriteState::FINISHED;  // Already done.
    }

    if (MOZ_UNLIKELY(!aSource)) {
      NS_WARNING("Passed a null pointer to WriteBuffer");
      return WriteState::FAILURE;
    }

    AdvanceRow(reinterpret_cast<const uint8_t*>(aSource));
    return IsSurfaceFinished() ? WriteState::FINISHED
                               : WriteState::NEED_MORE_DATA;
  }

  /**
   * Write a row to the surface by copying from a buffer. This is bounds checked
   * and memory safe with respect to the surface, but care must still be taken
   * by the caller not to overread the source buffer. This variant of
   * WriteBuffer() reads at most @aLength pixels from the buffer and writes them
   * to the row starting at @aStartColumn. Any pixels in columns before
   * @aStartColumn or after the pixels copied from the buffer are cleared.
   *
   * Bounds checking failures produce warnings in debug builds because although
   * the bounds checking maintains safety, this kind of failure could indicate a
   * bug in the calling code.
   *
   * The template parameter PixelType must be uint8_t (for paletted surfaces) or
   * uint32_t (for BGRA/BGRX surfaces) and must be in agreement with the pixel
   * size passed to ConfigureFilter().
   *
   * XXX(seth): We'll remove all support for paletted surfaces in bug 1247520,
   * which means we can remove the PixelType template parameter from this
   * method.
   *
   * @param aSource A buffer to copy from. This buffer must be @aLength pixels
   *                wide, which means |aLength * sizeof(PixelType)| bytes. May
   *                not be null.
   * @param aStartColumn The column to start writing to in the row. Columns
   *                     before this are cleared.
   * @param aLength The number of bytes, at most, which may be copied from
   *                @aSource. Fewer bytes may be copied in practice due to
   *                bounds checking.
   *
   * @return WriteState::FINISHED if the entire surface has been written to.
   *         Otherwise, returns WriteState::NEED_MORE_DATA. If a null |aSource|
   *         value is passed, returns WriteState::FAILURE.
   */
  template <typename PixelType>
  WriteState WriteBuffer(const PixelType* aSource, const size_t aStartColumn,
                         const size_t aLength) {
    MOZ_ASSERT(mPixelSize == 1 || mPixelSize == 4);
    MOZ_ASSERT_IF(mPixelSize == 1, sizeof(PixelType) == sizeof(uint8_t));
    MOZ_ASSERT_IF(mPixelSize == 4, sizeof(PixelType) == sizeof(uint32_t));

    if (IsSurfaceFinished()) {
      return WriteState::FINISHED;  // Already done.
    }

    if (MOZ_UNLIKELY(!aSource)) {
      NS_WARNING("Passed a null pointer to WriteBuffer");
      return WriteState::FAILURE;
    }

    PixelType* dest = reinterpret_cast<PixelType*>(mRowPointer);

    // Clear the area before |aStartColumn|.
    const size_t prefixLength =
        std::min<size_t>(mInputSize.width, aStartColumn);
    if (MOZ_UNLIKELY(prefixLength != aStartColumn)) {
      NS_WARNING("Provided starting column is out-of-bounds in WriteBuffer");
    }

    memset(dest, 0, mInputSize.width * sizeof(PixelType));
    dest += prefixLength;

    // Write |aLength| pixels from |aSource| into the row, with bounds checking.
    const size_t bufferLength =
        std::min<size_t>(mInputSize.width - prefixLength, aLength);
    if (MOZ_UNLIKELY(bufferLength != aLength)) {
      NS_WARNING("Provided buffer length is out-of-bounds in WriteBuffer");
    }

    memcpy(dest, aSource, bufferLength * sizeof(PixelType));
    dest += bufferLength;

    // Clear the rest of the row.
    const size_t suffixLength =
        mInputSize.width - (prefixLength + bufferLength);
    memset(dest, 0, suffixLength * sizeof(PixelType));

    AdvanceRow();

    return IsSurfaceFinished() ? WriteState::FINISHED
                               : WriteState::NEED_MORE_DATA;
  }

  /**
   * Write an empty row to the surface. If some pixels have already been written
   * to this row, they'll be discarded.
   *
   * @return WriteState::FINISHED if the entire surface has been written to.
   *         Otherwise, returns WriteState::NEED_MORE_DATA.
   */
  WriteState WriteEmptyRow() {
    if (IsSurfaceFinished()) {
      return WriteState::FINISHED;  // Already done.
    }

    memset(mRowPointer, 0, mInputSize.width * mPixelSize);
    AdvanceRow();

    return IsSurfaceFinished() ? WriteState::FINISHED
                               : WriteState::NEED_MORE_DATA;
  }

  /**
   * Write a row to the surface by calling a lambda that uses a pointer to
   * directly write to the row. This is unsafe because SurfaceFilter can't
   * provide any bounds checking; that's up to the lambda itself. For this
   * reason, the other Write*() methods should be preferred whenever it's
   * possible to use them; WriteUnsafeComputedRow() should be used only when
   * it's absolutely necessary to avoid extra copies or other performance
   * penalties.
   *
   * This method should never be exposed to SurfacePipe consumers; it's strictly
   * for use in SurfaceFilters. If external code needs this method, it should
   * probably be turned into a SurfaceFilter.
   *
   * The template parameter PixelType must be uint8_t (for paletted surfaces) or
   * uint32_t (for BGRA/BGRX surfaces) and must be in agreement with the pixel
   * size passed to ConfigureFilter().
   *
   * XXX(seth): We'll remove all support for paletted surfaces in bug 1247520,
   * which means we can remove the PixelType template parameter from this
   * method.
   *
   * @param aFunc A lambda that writes directly to the row.
   *
   * @return WriteState::FINISHED if the entire surface has been written to.
   *         Otherwise, returns WriteState::NEED_MORE_DATA.
   */
  template <typename PixelType, typename Func>
  WriteState WriteUnsafeComputedRow(Func aFunc) {
    MOZ_ASSERT(mPixelSize == 1 || mPixelSize == 4);
    MOZ_ASSERT_IF(mPixelSize == 1, sizeof(PixelType) == sizeof(uint8_t));
    MOZ_ASSERT_IF(mPixelSize == 4, sizeof(PixelType) == sizeof(uint32_t));

    if (IsSurfaceFinished()) {
      return WriteState::FINISHED;  // Already done.
    }

    // Call the provided lambda with a pointer to the buffer for the current
    // row. This is unsafe because we can't do any bounds checking; the lambda
    // itself has to be responsible for that.
    PixelType* rowPtr = reinterpret_cast<PixelType*>(mRowPointer);
    aFunc(rowPtr, mInputSize.width);
    AdvanceRow();

    return IsSurfaceFinished() ? WriteState::FINISHED
                               : WriteState::NEED_MORE_DATA;
  }

  //////////////////////////////////////////////////////////////////////////////
  // Methods Subclasses Should Override
  //////////////////////////////////////////////////////////////////////////////

  /**
   * @return a SurfaceInvalidRect representing the region of the surface that
   *         has been written to since the last time TakeInvalidRect() was
   *         called, or Nothing() if the region is empty (i.e. nothing has been
   *         written).
   */
  virtual Maybe<SurfaceInvalidRect> TakeInvalidRect() = 0;

 protected:
  /**
   * Called by ResetToFirstRow() to actually perform the reset. It's legal to
   * throw away any previously written data at this point, as all rows must be
   * written to on every pass.
   */
  virtual uint8_t* DoResetToFirstRow() = 0;

  /**
   * Called by AdvanceRow() to actually advance this filter to the next row.
   *
   * @param aInputRow The input row supplied by the decoder.
   *
   * @return a pointer to the buffer for the next row, or nullptr to indicate
   *         that we've finished the entire surface.
   */
  virtual uint8_t* DoAdvanceRowFromBuffer(const uint8_t* aInputRow) = 0;

  /**
   * Called by AdvanceRow() to actually advance this filter to the next row.
   *
   * @return a pointer to the buffer for the next row, or nullptr to indicate
   *         that we've finished the entire surface.
   */
  virtual uint8_t* DoAdvanceRow() = 0;

  //////////////////////////////////////////////////////////////////////////////
  // Methods For Internal Use By Subclasses
  //////////////////////////////////////////////////////////////////////////////

  /**
   * Called by subclasses' Configure() methods to initialize the configuration
   * of this filter. After the filter is configured, calls ResetToFirstRow().
   *
   * @param aInputSize The input size of this filter, in pixels. The previous
   *                   filter in the chain will expect to write into rows
   *                   |aInputSize.width| pixels wide.
   * @param aPixelSize How large, in bytes, each pixel in the surface is. This
   *                   should be either 1 for paletted images or 4 for BGRA/BGRX
   *                   images.
   */
  void ConfigureFilter(gfx::IntSize aInputSize, uint8_t aPixelSize) {
    mInputSize = aInputSize;
    mPixelSize = aPixelSize;

    ResetToFirstRow();
  }

  /**
   * Called by subclasses' DoAdvanceRowFromBuffer() methods to copy a decoder
   * supplied row buffer into its internal row pointer. Ideally filters at the
   * top of the filter pipeline are able to consume the decoder row buffer
   * directly without the extra copy prior to performing its transformation.
   *
   * @param aInputRow The input row supplied by the decoder.
   */
  void CopyInputRow(const uint8_t* aInputRow) {
    MOZ_ASSERT(aInputRow);
    MOZ_ASSERT(mCol == 0);
    memcpy(mRowPointer, aInputRow, mPixelSize * mInputSize.width);
  }

 private:
  /**
   * An internal method used to implement WritePixelBlocks. This method writes
   * up to the number of pixels necessary to complete the row and returns Some()
   * if we either finished the entire surface or the lambda returned a
   * WriteState indicating that we should return to the caller. If the row was
   * successfully written without either of those things happening, it returns
   * Nothing(), allowing WritePixelBlocks() to iterate to fill as many rows as
   * possible.
   */
  template <typename PixelType, typename Func>
  Maybe<WriteState> DoWritePixelBlockToRow(Func aFunc) {
    MOZ_ASSERT(mPixelSize == 1 || mPixelSize == 4);
    MOZ_ASSERT_IF(mPixelSize == 1, sizeof(PixelType) == sizeof(uint8_t));
    MOZ_ASSERT_IF(mPixelSize == 4, sizeof(PixelType) == sizeof(uint32_t));

    if (IsSurfaceFinished()) {
      return Some(WriteState::FINISHED);  // We're already done.
    }

    PixelType* rowPtr = reinterpret_cast<PixelType*>(mRowPointer);
    int32_t remainder = mInputSize.width - mCol;
    int32_t written;
    Maybe<WriteState> result;
    Tie(written, result) = aFunc(&rowPtr[mCol], remainder);
    if (written == remainder) {
      MOZ_ASSERT(result.isNothing());
      mCol = mInputSize.width;
      AdvanceRow();  // We've finished the row.
      return IsSurfaceFinished() ? Some(WriteState::FINISHED) : Nothing();
    }

    MOZ_ASSERT(written >= 0 && written < remainder);
    MOZ_ASSERT(result.isSome());

    mCol += written;
    if (*result == WriteState::FINISHED) {
      ZeroOutRestOfSurface<PixelType>();
    }

    return result;
  }

  /**
   * An internal method used to implement both WritePixels() and
   * WritePixelsToRow(). Those methods differ only in their behavior after a row
   * is successfully written - WritePixels() continues to write another row,
   * while WritePixelsToRow() returns to the caller. This method writes a single
   * row and returns Some() if we either finished the entire surface or the
   * lambda returned a WriteState indicating that we should return to the
   * caller. If the row was successfully written without either of those things
   * happening, it returns Nothing(), allowing WritePixels() and
   * WritePixelsToRow() to implement their respective behaviors.
   */
  template <typename PixelType, typename Func>
  Maybe<WriteState> DoWritePixelsToRow(Func aFunc) {
    MOZ_ASSERT(mPixelSize == 1 || mPixelSize == 4);
    MOZ_ASSERT_IF(mPixelSize == 1, sizeof(PixelType) == sizeof(uint8_t));
    MOZ_ASSERT_IF(mPixelSize == 4, sizeof(PixelType) == sizeof(uint32_t));

    if (IsSurfaceFinished()) {
      return Some(WriteState::FINISHED);  // We're already done.
    }

    PixelType* rowPtr = reinterpret_cast<PixelType*>(mRowPointer);

    for (; mCol < mInputSize.width; ++mCol) {
      NextPixel<PixelType> result = aFunc();
      if (result.template is<PixelType>()) {
        rowPtr[mCol] = result.template as<PixelType>();
        continue;
      }

      switch (result.template as<WriteState>()) {
        case WriteState::NEED_MORE_DATA:
          return Some(WriteState::NEED_MORE_DATA);

        case WriteState::FINISHED:
          ZeroOutRestOfSurface<PixelType>();
          return Some(WriteState::FINISHED);

        case WriteState::FAILURE:
          // Note that we don't need to record this anywhere, because this
          // indicates an error in aFunc, and there's nothing wrong with our
          // machinery. The caller can recover as needed and continue writing to
          // the row.
          return Some(WriteState::FAILURE);
      }
    }

    AdvanceRow();  // We've finished the row.

    return IsSurfaceFinished() ? Some(WriteState::FINISHED) : Nothing();
  }

  template <typename PixelType>
  void ZeroOutRestOfSurface() {
    WritePixels<PixelType>([] { return AsVariant(PixelType(0)); });
  }

  gfx::IntSize mInputSize;  /// The size of the input this filter expects.
  uint8_t* mRowPointer;     /// Pointer to the current row or null if finished.
  int32_t mCol;             /// The current column we're writing to. (0-indexed)
  uint8_t mPixelSize;  /// How large each pixel in the surface is, in bytes.
};

/**
 * SurfacePipe is the public API that decoders should use to interact with a
 * SurfaceFilter pipeline.
 */
class SurfacePipe {
 public:
  SurfacePipe() {}

  SurfacePipe(SurfacePipe&& aOther) : mHead(std::move(aOther.mHead)) {}

  ~SurfacePipe() {}

  SurfacePipe& operator=(SurfacePipe&& aOther) {
    MOZ_ASSERT(this != &aOther);
    mHead = std::move(aOther.mHead);
    return *this;
  }

  /// Begins a new pass, seeking to the first row of the surface.
  void ResetToFirstRow() {
    MOZ_ASSERT(mHead, "Use before configured!");
    mHead->ResetToFirstRow();
  }

  /**
   * Write pixels to the surface one at a time by repeatedly calling a lambda
   * that yields pixels. WritePixels() is completely memory safe.
   *
   * @see SurfaceFilter::WritePixels() for the canonical documentation.
   */
  template <typename PixelType, typename Func>
  WriteState WritePixels(Func aFunc) {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->WritePixels<PixelType>(std::forward<Func>(aFunc));
  }

  /**
   * A variant of WritePixels() that writes up to a single row of pixels to the
   * surface in blocks by repeatedly calling a lambda that yields up to the
   * requested number of pixels.
   *
   * @see SurfaceFilter::WritePixelBlocks() for the canonical documentation.
   */
  template <typename PixelType, typename Func>
  WriteState WritePixelBlocks(Func aFunc) {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->WritePixelBlocks<PixelType>(std::forward<Func>(aFunc));
  }

  /**
   * A variant of WritePixels() that writes a single row of pixels to the
   * surface one at a time by repeatedly calling a lambda that yields pixels.
   * WritePixelsToRow() is completely memory safe.
   *
   * @see SurfaceFilter::WritePixelsToRow() for the canonical documentation.
   */
  template <typename PixelType, typename Func>
  WriteState WritePixelsToRow(Func aFunc) {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->WritePixelsToRow<PixelType>(std::forward<Func>(aFunc));
  }

  /**
   * Write a row to the surface by copying from a buffer. This is bounds checked
   * and memory safe with respect to the surface, but care must still be taken
   * by the caller not to overread the source buffer. This variant of
   * WriteBuffer() requires a source buffer which contains |mInputSize.width|
   * pixels.
   *
   * @see SurfaceFilter::WriteBuffer() for the canonical documentation.
   */
  template <typename PixelType>
  WriteState WriteBuffer(const PixelType* aSource) {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->WriteBuffer<PixelType>(aSource);
  }

  /**
   * Write a row to the surface by copying from a buffer. This is bounds checked
   * and memory safe with respect to the surface, but care must still be taken
   * by the caller not to overread the source buffer. This variant of
   * WriteBuffer() reads at most @aLength pixels from the buffer and writes them
   * to the row starting at @aStartColumn. Any pixels in columns before
   * @aStartColumn or after the pixels copied from the buffer are cleared.
   *
   * @see SurfaceFilter::WriteBuffer() for the canonical documentation.
   */
  template <typename PixelType>
  WriteState WriteBuffer(const PixelType* aSource, const size_t aStartColumn,
                         const size_t aLength) {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->WriteBuffer<PixelType>(aSource, aStartColumn, aLength);
  }

  /**
   * Write an empty row to the surface. If some pixels have already been written
   * to this row, they'll be discarded.
   *
   * @see SurfaceFilter::WriteEmptyRow() for the canonical documentation.
   */
  WriteState WriteEmptyRow() {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->WriteEmptyRow();
  }

  /// @return true if we've finished writing to the surface.
  bool IsSurfaceFinished() const { return mHead->IsSurfaceFinished(); }

  /// @see SurfaceFilter::TakeInvalidRect() for the canonical documentation.
  Maybe<SurfaceInvalidRect> TakeInvalidRect() const {
    MOZ_ASSERT(mHead, "Use before configured!");
    return mHead->TakeInvalidRect();
  }

 private:
  friend class SurfacePipeFactory;
  friend class TestSurfacePipeFactory;

  explicit SurfacePipe(UniquePtr<SurfaceFilter>&& aHead)
      : mHead(std::move(aHead)) {}

  SurfacePipe(const SurfacePipe&) = delete;
  SurfacePipe& operator=(const SurfacePipe&) = delete;

  UniquePtr<SurfaceFilter> mHead;  /// The first filter in the chain.
};

/**
 * AbstractSurfaceSink contains shared implementation for both SurfaceSink and
 * ReorientSurfaceSink.
 */
class AbstractSurfaceSink : public SurfaceFilter {
 public:
  AbstractSurfaceSink()
      : mImageData(nullptr),
        mImageDataLength(0),
        mRow(0),
        mFlipVertically(false) {}

  Maybe<SurfaceInvalidRect> TakeInvalidRect() final;

 protected:
  uint8_t* DoResetToFirstRow() final;
  virtual uint8_t* GetRowPointer() const = 0;

  OrientedIntRect
      mInvalidRect;     /// The region of the surface that has been written
                        /// to since the last call to TakeInvalidRect().
  uint8_t* mImageData;  /// A pointer to the beginning of the surface data.
  uint32_t mImageDataLength;  /// The length of the surface data.
  uint32_t mRow;              /// The row to which we're writing. (0-indexed)
  bool mFlipVertically;       /// If true, write the rows from top to bottom.
};

class SurfaceSink;

/// A configuration struct for SurfaceSink.
struct SurfaceConfig {
  using Filter = SurfaceSink;
  Decoder* mDecoder;           /// Which Decoder to use to allocate the surface.
  gfx::IntSize mOutputSize;    /// The size of the surface.
  gfx::SurfaceFormat mFormat;  /// The surface format (BGRA or BGRX).
  bool mFlipVertically;        /// If true, write the rows from bottom to top.
  Maybe<AnimationParams> mAnimParams;  /// Given for animated images.
};

/**
 * A sink for surfaces. It handles the allocation of the surface and protects
 * against buffer overflow. This sink should be used for most images.
 *
 * Sinks must always be at the end of the SurfaceFilter chain.
 */
class SurfaceSink final : public AbstractSurfaceSink {
 public:
  nsresult Configure(const SurfaceConfig& aConfig);

 protected:
  uint8_t* DoAdvanceRowFromBuffer(const uint8_t* aInputRow) final;
  uint8_t* DoAdvanceRow() final;
  uint8_t* GetRowPointer() const final;
};

class ReorientSurfaceSink;

/// A configuration struct for ReorientSurfaceSink.
struct ReorientSurfaceConfig {
  using Filter = ReorientSurfaceSink;
  Decoder* mDecoder;  /// Which Decoder to use to allocate the surface.
  OrientedIntSize mOutputSize;  /// The size of the surface.
  gfx::SurfaceFormat mFormat;   /// The surface format (BGRA or BGRX).
  Orientation mOrientation;     /// The desired orientation of the surface data.
};

/**
 * A sink for surfaces. It handles the allocation of the surface and protects
 * against buffer overflow. This sink should be used for images which have a
 * non-identity orientation which we want to apply during decoding.
 *
 * Sinks must always be at the end of the SurfaceFilter chain.
 */
class ReorientSurfaceSink final : public AbstractSurfaceSink {
 public:
  nsresult Configure(const ReorientSurfaceConfig& aConfig);

 protected:
  uint8_t* DoAdvanceRowFromBuffer(const uint8_t* aInputRow) final;
  uint8_t* DoAdvanceRow() final;
  uint8_t* GetRowPointer() const final;

  UniquePtr<uint8_t[]> mBuffer;
  gfx::ReorientRowFn mReorientFn;
  gfx::IntSize mSurfaceSize;
};

}  // namespace image
}  // namespace mozilla

#endif  // mozilla_image_SurfacePipe_h
