#include "zstd.h"
#include "codec.h"
#include "dictionary_codec.h"
#include "private.h"

#include <yt/yt/core/misc/finally.h>

#include <library/cpp/yt/system/exit.h>

#include <library/cpp/yt/memory/blob.h>
#include <library/cpp/yt/memory/chunked_memory_pool.h>

#include <util/system/unaligned_mem.h>

#include <contrib/libs/zstd/lib/zstd_errors.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <contrib/libs/zstd/include/zstd.h>
#define ZDICT_STATIC_LINKING_ONLY
#include <contrib/libs/zstd/include/zdict.h>

namespace NYT::NCompression::NDetail {

////////////////////////////////////////////////////////////////////////////////

constinit const auto Logger = CompressionLogger;

////////////////////////////////////////////////////////////////////////////////

struct TZstdCompressBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

void VerifyError(size_t result)
{
    if (!ZSTD_isError(result)) {
        return;
    }

    if (result == ZSTD_error_memory_allocation) {
        AbortProcessDramatically(
            EProcessExitCode::OutOfMemory,
            "Zstd codec failed with memory allocation error");
    }

    YT_LOG_FATAL("Zstd compression failed (Error: %v)",
        ZSTD_getErrorName(result));
}

////////////////////////////////////////////////////////////////////////////////

void ZstdCompress(int level, TSource* source, TBlob* output)
{
    ui64 totalInputSize = source->Available();
    output->Resize(ZSTD_compressBound(totalInputSize) + sizeof(totalInputSize), /*initializeStorage*/ false);
    size_t curOutputPos = 0;

    // Write input size that will be used during decompression.
    {
        TMemoryOutput memoryOutput(output->Begin(), sizeof(totalInputSize));
        WritePod(memoryOutput, totalInputSize);
        curOutputPos += sizeof(totalInputSize);
    }

    auto context = ZSTD_createCCtx();
    auto contextGuard = Finally([&] {
        ZSTD_freeCCtx(context);
    });

    {
        auto result = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
        VerifyError(result);
    }

    struct CompressResult
    {
        size_t Result;
        size_t ProcessedBytes;
    };

    auto compressToOutput = [&] (const char* buffer, size_t size, bool isLastBlock) -> CompressResult {
        ZSTD_EndDirective mode = isLastBlock ? ZSTD_e_end : ZSTD_e_continue;

        ZSTD_inBuffer zstdInput = {buffer, size, 0};
        ZSTD_outBuffer zstdOutput = {output->Begin(), output->Size(), curOutputPos};

        size_t result = ZSTD_compressStream2(context, &zstdOutput, &zstdInput, mode);
        VerifyError(result);
        curOutputPos = zstdOutput.pos;

        return CompressResult{
            .Result = result,
            .ProcessedBytes = zstdInput.pos,
        };
    };

    auto recommendedInputSize = ZSTD_CStreamInSize();

    auto block = TBlob(GetRefCountedTypeCookie<TZstdCompressBufferTag>());
    size_t blockSize = 0;

    auto compressDataFromBlock = [&] (bool isLastBlock) {
        size_t remainingSize = blockSize;
        while (remainingSize > 0) {
            auto compressResult = compressToOutput(
                block.Begin() + (blockSize - remainingSize),
                remainingSize,
                isLastBlock);
            remainingSize -= compressResult.ProcessedBytes;
            if (isLastBlock ? compressResult.Result == 0 : remainingSize == 0) {
                break;
            }
        }
        blockSize = 0;
    };

    while (source->Available() > 0) {
        size_t inputSize;
        const char* inputPtr = source->Peek(&inputSize);

        bool shouldFlushBlock = false;
        if (inputSize < recommendedInputSize) {
            if (block.IsEmpty()) {
                block.Resize(recommendedInputSize, /*initializeStorage*/ false);
            }
            if (blockSize + inputSize <= block.Size()) {
                // Append to block.
                memcpy(block.Begin() + blockSize, inputPtr, inputSize);
                blockSize += inputSize;
                source->Skip(inputSize);
                continue;
            } else {
                shouldFlushBlock = true;
            }
        } else {
            shouldFlushBlock = blockSize > 0;
        }

        if (shouldFlushBlock) {
            compressDataFromBlock(/*isLastBlock*/ false);
        }

        size_t remainingSize = inputSize;

        bool isLastBlock = source->Available() == remainingSize;
        while (true) {
            auto compressResult = compressToOutput(
                inputPtr + (inputSize - remainingSize),
                remainingSize,
                isLastBlock);
            source->Skip(compressResult.ProcessedBytes);
            remainingSize -= compressResult.ProcessedBytes;
            if (isLastBlock ? compressResult.Result == 0 : remainingSize == 0) {
                break;
            }
        }
    }

    if (blockSize > 0) {
        compressDataFromBlock(/*isLastBlock*/ true);
    }

    output->Resize(curOutputPos);
}

void ZstdDecompress(TSource* source, TBlob* output)
{
    ui64 outputSize;
    ReadPod(*source, outputSize);

    output->Resize(outputSize, /*initializeStorage*/ false);
    void* outputPtr = output->Begin();

    size_t inputSize;
    const void* inputPtr = source->Peek(&inputSize);

    // TODO(babenko): something weird is going on here.
    TBlob input;
    if (auto available = source->Available(); available > inputSize) {
        input.Resize(available, /*initializeStorage*/ false);
        ReadRef(*source, TMutableRef(input.Begin(), available));
        inputPtr = input.Begin();
        inputSize = input.Size();
    }

    size_t decompressedSize = ZSTD_decompress(outputPtr, outputSize, inputPtr, inputSize);
    if (ZSTD_isError(decompressedSize)) {
        THROW_ERROR_EXCEPTION("Zstd decompression failed: ZSTD_decompress returned an error")
            << TErrorAttribute("error", ZSTD_getErrorName(decompressedSize));
    }
    if (decompressedSize != outputSize) {
        THROW_ERROR_EXCEPTION("Zstd decompression failed: output size mismatch")
            << TErrorAttribute("expected_size", outputSize)
            << TErrorAttribute("actual_size", decompressedSize);
    }
}

////////////////////////////////////////////////////////////////////////////////

// ZstdSyncTagPrefix + 64-bit offset
static_assert(ZstdSyncTagPrefixSize + sizeof(ui64) == ZstdSyncTagSize);

EZstdFrameType DetectZstdFrameType(TRef buffer)
{
    YT_VERIFY(buffer.Size() >= ZstdFrameSignatureSize);
    auto signatureStr = TStringBuf(buffer.begin(), ZstdFrameSignatureSize);
    if (signatureStr == ZstdSyncFrameSignature) {
        return EZstdFrameType::Sync;
    }
    if (signatureStr == ZstdDataFrameSignature) {
        return EZstdFrameType::Data;
    }
    return EZstdFrameType::Unknown;
}

std::optional<i64> FindLastZstdSyncTagOffset(TRef buffer, i64 bufferStartOffset)
{
    std::optional<i64> result;
    auto currentBufferStr = TStringBuf(buffer.data(), buffer.size());
    while (true) {
        size_t tagPos = currentBufferStr.find(ZstdSyncTagPrefix);
        if (tagPos == TStringBuf::npos) {
            break;
        }

        const char* tag = currentBufferStr.data() + tagPos;
        if (currentBufferStr.end() - tag < ZstdSyncTagSize) {
            break;
        }

        ui64 tagOffset = ReadUnaligned<ui64>(tag + ZstdSyncTagPrefix.size());
        ui64 tagOffsetExpected = bufferStartOffset + (tag - buffer.data());
        if (tagOffset == tagOffsetExpected) {
            result = tagOffset;
        }

        currentBufferStr.remove_prefix(tagPos + ZstdSyncTagPrefix.size());
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

int ZstdGetMinDictionarySize()
{
    return ZDICT_DICTSIZE_MIN;
}

int ZstdGetMaxCompressionLevel()
{
    return ZSTD_maxCLevel();
}

int ZstdGetDefaultCompressionLevel()
{
    return ZSTD_defaultCLevel();
}

TDictionaryCompressionFrameInfo ZstdGetFrameInfo(TRef input)
{
    YT_VERIFY(input.Size() >= ZSTD_FRAMEHEADERSIZE_MIN(ZSTD_f_zstd1_magicless));

    ZSTD_frameHeader frameHeader;
    auto result = ZSTD_getFrameHeader_advanced(
        &frameHeader,
        input.Begin(),
        input.Size(),
        ZSTD_f_zstd1_magicless);
    if (result != 0) {
        THROW_ERROR_EXCEPTION("Failed to get frame header")
            << TErrorAttribute("code", result)
            << TErrorAttribute("is_error", ZSTD_isError(result))
            << TErrorAttribute("error", ZSTD_getErrorName(result));
    }

    return {
        .ContentSize = frameHeader.frameContentSize,
    };
}

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TDigestedCompressionDictionary)

class TDigestedCompressionDictionary
    : public IDigestedCompressionDictionary
    , private TNonCopyable
{
public:
    explicit TDigestedCompressionDictionary(
        TSharedRef storage,
        const ZSTD_CDict* digestedDictionary)
        : Storage_(std::move(storage))
        , DigestedDictionary_(digestedDictionary)
    {
        YT_VERIFY(Storage_);
        YT_VERIFY(DigestedDictionary_);
    }

    i64 GetMemoryUsage() const override
    {
        return ZSTD_sizeof_CDict(DigestedDictionary_);
    }

    const ZSTD_CDict* GetDigestedDictionary() const
    {
        return DigestedDictionary_;
    }

private:
    //! NB: DigestedDictionary is initialized over preallocated memory referenced by Storage.
    //! For that reason we must assure that Storage lifetime is longer and also do not need to call ZSTD_freeCDict.
    const TSharedRef Storage_;
    const ZSTD_CDict* const DigestedDictionary_;
};

DEFINE_REFCOUNTED_TYPE(TDigestedCompressionDictionary)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TDigestedDecompressionDictionary)

class TDigestedDecompressionDictionary
    : public IDigestedDecompressionDictionary
    , private TNonCopyable
{
public:
    explicit TDigestedDecompressionDictionary(
        TSharedRef storage,
        const ZSTD_DDict* digestedDictionary)
        : Storage_(std::move(storage))
        , DigestedDictionary_(digestedDictionary)
    {
        YT_VERIFY(Storage_);
        YT_VERIFY(DigestedDictionary_);
    }

    i64 GetMemoryUsage() const override
    {
        return ZSTD_sizeof_DDict(DigestedDictionary_);
    }

    const ZSTD_DDict* GetDigestedDictionary() const
    {
        return DigestedDictionary_;
    }

private:
    //! NB: DigestedDictionary is initialized over preallocated memory referenced by Storage.
    //! For that reason we must assure that Storage lifetime is longer and also do not need to call ZSTD_freeDDict.
    const TSharedRef Storage_;
    const ZSTD_DDict* const DigestedDictionary_;
};

DEFINE_REFCOUNTED_TYPE(TDigestedDecompressionDictionary)

////////////////////////////////////////////////////////////////////////////////

class TDictionaryCompressionContextGuard
    : private TMoveOnly
{
public:
    TDictionaryCompressionContextGuard()
        : Context_(ZSTD_createCCtx())
    { }

    TDictionaryCompressionContextGuard(TDictionaryCompressionContextGuard&& other)
        : Context_(other.Context_)
    {
        other.Context_ = nullptr;
    }

    TDictionaryCompressionContextGuard& operator = (TDictionaryCompressionContextGuard&& other) = delete;

    ~TDictionaryCompressionContextGuard()
    {
        if (Context_) {
            ZSTD_freeCCtx(Context_);
        }
    }

    ZSTD_CCtx* GetContext() const
    {
        return Context_;
    }

private:
    ZSTD_CCtx* Context_;
};

////////////////////////////////////////////////////////////////////////////////

class TDictionaryDecompressionContextGuard
    : private TMoveOnly
{
public:
    TDictionaryDecompressionContextGuard()
        : Context_(ZSTD_createDCtx())
    { }

    TDictionaryDecompressionContextGuard(TDictionaryDecompressionContextGuard&& other)
        : Context_(other.Context_)
    {
        other.Context_ = nullptr;
    }

    TDictionaryDecompressionContextGuard& operator = (TDictionaryDecompressionContextGuard&& other) = delete;

    ~TDictionaryDecompressionContextGuard()
    {
        if (Context_) {
            ZSTD_freeDCtx(Context_);
        }
    }

    ZSTD_DCtx* GetContext() const
    {
        return Context_;
    }

private:
    ZSTD_DCtx* Context_;
};

////////////////////////////////////////////////////////////////////////////////

class TDictionaryCompressor
    : public IDictionaryCompressor
{
public:
    TDictionaryCompressor(
        TDictionaryCompressionContextGuard context,
        TDigestedCompressionDictionaryPtr digestedDictionary)
        : Context_(std::move(context))
        , DigestedDictionary_(std::move(digestedDictionary))
    { }

    TRef Compress(
        TChunkedMemoryPool* pool,
        TRef input) override
    {
        auto maxSize = ZSTD_compressBound(input.Size());
        char* output = pool->AllocateUnaligned(maxSize);

        auto actualSize = ZSTD_compress2(
            Context_.GetContext(),
            output,
            maxSize,
            input.Begin(),
            input.Size());
        VerifyError(actualSize);
        YT_VERIFY(actualSize <= maxSize);
        pool->Free(output + actualSize, output + maxSize);

        return TRef(output, actualSize);
    }

private:
    const TDictionaryCompressionContextGuard Context_;
    const TDigestedCompressionDictionaryPtr DigestedDictionary_;
};

////////////////////////////////////////////////////////////////////////////////

class TDictionaryDecompressor
    : public IDictionaryDecompressor
{
public:
    TDictionaryDecompressor(
        TDictionaryDecompressionContextGuard context,
        TDigestedDecompressionDictionaryPtr digestedDictionary)
        : Context_(std::move(context))
        , DigestedDictionary_(std::move(digestedDictionary))
    { }

    void Decompress(
        TRef input,
        TMutableRef output) override
    {
        auto decompressedSize = ZSTD_decompressDCtx(
            Context_.GetContext(),
            output.Begin(),
            output.Size(),
            input.Begin(),
            input.Size());
        VerifyError(decompressedSize);
        YT_VERIFY(decompressedSize == output.Size());
    }

private:
    const TDictionaryDecompressionContextGuard Context_;
    const TDigestedDecompressionDictionaryPtr DigestedDictionary_;
};

////////////////////////////////////////////////////////////////////////////////

struct TCompressionDictionaryBlobTag
{ };

struct TCompressionDictionarySamplesBlobTag
{ };

TErrorOr<TSharedRef> ZstdTrainCompressionDictionary(i64 dictionarySize, const std::vector<TSharedRef>& samples)
{
    TBlob dictionary(
        GetRefCountedTypeCookie<TCompressionDictionaryBlobTag>(),
        /*size*/ dictionarySize,
        /*initiailizeStorage*/ false,
        /*pageAligned*/ true);

    std::vector<size_t> sampleSizes;
    sampleSizes.reserve(samples.size());
    for (const auto& sample : samples) {
        sampleSizes.push_back(sample.size());
    }

    auto mergedSamples = samples.size() == 1
        ? samples[0]
        : MergeRefsToRef<TCompressionDictionarySamplesBlobTag>(samples);

    auto resultDictionarySize = ZDICT_trainFromBuffer(
        dictionary.Begin(),
        dictionarySize,
        mergedSamples.Begin(),
        sampleSizes.data(),
        sampleSizes.size());
    if (ZSTD_isError(resultDictionarySize)) {
        return TError("Compression dictionary training failed")
            << TErrorAttribute("zstd_error_code", static_cast<int>(ZSTD_getErrorCode(resultDictionarySize)))
            << TErrorAttribute("zstd_error_name", ZSTD_getErrorName(resultDictionarySize));
    }

    YT_VERIFY(resultDictionarySize <= dictionary.Size());
    dictionary.Resize(resultDictionarySize);

    return TSharedRef::FromBlob(std::move(dictionary));
}

////////////////////////////////////////////////////////////////////////////////

i64 ZstdEstimateDigestedCompressionDictionarySize(i64 dictionarySize, int compressionLevel)
{
    return ZSTD_estimateCDictSize(dictionarySize, compressionLevel);
}

i64 ZstdEstimateDigestedDecompressionDictionarySize(i64 dictionarySize)
{
    return ZSTD_estimateDDictSize(dictionarySize, ZSTD_dlm_byCopy);
}

IDigestedCompressionDictionaryPtr ZstdConstructDigestedCompressionDictionary(
    const TSharedRef& compressionDictionary,
    TSharedMutableRef storage,
    int compressionLevel)
{
    YT_VERIFY(compressionDictionary);
    YT_VERIFY(storage);
    YT_VERIFY(compressionLevel >= 0 && compressionLevel <= ZstdGetMaxCompressionLevel());
    YT_VERIFY(AlignUp(storage.Begin(), 8ull) == storage.Begin());

    // XXX(akozhikhov): We have to assign #srcSizeHint here to 1 instead of 0 (as a default unknown value)
    // because of the mode that this function sets upon calling its internal implementation.
    // According to documentation 'ZSTD_cParamMode_e::ZSTD_cpm_createCDict' and 'ZSTD_cParamMode_e::ZSTD_cpm_unknown' seems to
    // have the same effect, however for some reason this is not true in case of zero #srcSizeHint.
    auto compressionParams = ZSTD_getCParams(
        compressionLevel,
        /*srcSizeHint*/ 1,
        compressionDictionary.Size());

    auto* digestedDictionary = ZSTD_initStaticCDict(
        storage.Begin(),
        storage.Size(),
        compressionDictionary.Begin(),
        compressionDictionary.Size(),
        ZSTD_dlm_byCopy,
        ZSTD_dct_auto,
        compressionParams);

    if (!digestedDictionary) {
        THROW_ERROR_EXCEPTION("Failed to create digested compression dictionary")
            << TErrorAttribute("compression_level", compressionLevel)
            << TErrorAttribute("dictionary_size", compressionDictionary.Size())
            << TErrorAttribute("storage_size", storage.Size());
    }

    return New<TDigestedCompressionDictionary>(std::move(storage), digestedDictionary);
}

IDigestedDecompressionDictionaryPtr ZstdConstructDigestedDecompressionDictionary(
    const TSharedRef& decompressionDictionary,
    TSharedMutableRef storage)
{
    YT_VERIFY(decompressionDictionary);
    YT_VERIFY(storage);

    auto* digestedDictionary = ZSTD_initStaticDDict(
        storage.Begin(),
        storage.Size(),
        decompressionDictionary.Begin(),
        decompressionDictionary.Size(),
        ZSTD_dlm_byCopy,
        ZSTD_dct_auto);

    if (!digestedDictionary) {
        THROW_ERROR_EXCEPTION("Failed to create digested decompression dictionary")
            << TErrorAttribute("dictionary_size", decompressionDictionary.Size())
            << TErrorAttribute("storage_size", storage.Size());
    }

    return New<TDigestedDecompressionDictionary>(std::move(storage), digestedDictionary);
}

////////////////////////////////////////////////////////////////////////////////

IDictionaryCompressorPtr ZstdCreateDictionaryCompressor(
    const IDigestedCompressionDictionaryPtr& digestedCompressionDictionary)
{
    YT_VERIFY(digestedCompressionDictionary);
    auto* typedDictionary = dynamic_cast<TDigestedCompressionDictionary*>(digestedCompressionDictionary.Get());
    YT_VERIFY(typedDictionary);

    TDictionaryCompressionContextGuard context;

    // Omit writing dictID and checksum to frame header.
    ZSTD_frameParameters frameParameters{
        .contentSizeFlag = 1,
        .checksumFlag = 0,
        .noDictIDFlag = 1,
    };
    VerifyError(ZSTD_CCtx_setFParams(context.GetContext(), frameParameters));

    // Omit writing magic number to frame header.
    // NB: This parameter must remain intact for compressor-decompressor compatibility.
    VerifyError(ZSTD_CCtx_setParameter(context.GetContext(), ZSTD_c_format, ZSTD_f_zstd1_magicless));

    // Omit copying digested dictionary content.
    VerifyError(ZSTD_CCtx_refCDict(context.GetContext(), typedDictionary->GetDigestedDictionary()));

    return New<TDictionaryCompressor>(std::move(context), typedDictionary);
}

IDictionaryDecompressorPtr ZstdCreateDictionaryDecompressor(
    const IDigestedDecompressionDictionaryPtr& digestedDecompressionDictionary)
{
    YT_VERIFY(digestedDecompressionDictionary);
    auto* typedDictionary = dynamic_cast<TDigestedDecompressionDictionary*>(digestedDecompressionDictionary.Get());
    YT_VERIFY(typedDictionary);

    TDictionaryDecompressionContextGuard context;

    VerifyError(ZSTD_DCtx_setParameter(context.GetContext(), ZSTD_d_format, ZSTD_f_zstd1_magicless));

    VerifyError(ZSTD_DCtx_refDDict(context.GetContext(), typedDictionary->GetDigestedDictionary()));

    return New<TDictionaryDecompressor>(std::move(context), typedDictionary);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCompression::NDetail

