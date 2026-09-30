#include "Audio/Decoder/DecoderFactory.h"
#include "Audio/Decoder/Detail/Decoders.h"
#include "Audio/Core/AudioEngine.h"
#include "Audio/Core/Resample.h"
#include "Audio/SampleProvider/MemorySampleProvider.h"
#include "IO/File.h"
#include "IO/Path.h"
#include "Misc/StringUtil.h"
#include "Core/Logger.h"


namespace Comfy::Audio
{
	std::unique_ptr<DecoderFactory> DecoderFactoryInstance = std::make_unique<DecoderFactory>();

	DecoderFactory::DecoderFactory()
	{
		RegisterAllDecoders();
	}

	DecoderFactory& DecoderFactory::GetInstance()
	{
		return *DecoderFactoryInstance;
	}

	std::unique_ptr<ISampleProvider> DecoderFactory::DecodeFile(std::string_view filePath)
	{
		if (!IO::File::Exists(filePath))
		{
			Logger::LogErrorLine("(): Input file %.*s not found", filePath.size(), filePath.data());
			return nullptr;
		}

		const auto extension = IO::Path::GetExtension(filePath);
		for (auto& decoder : availableDecoders)
		{
			if (!IO::Path::DoesAnyPackedExtensionMatch(extension, decoder->GetFileExtensions()))
				continue;

			const auto[fileContent, fileSize] = IO::File::ReadAllBytes(filePath);
			if (fileContent == nullptr)
			{
				Logger::LogErrorLine("(): Unable to read input file %.*s", filePath.size(), filePath.data());
				return nullptr;
			}

			return DecodeAndProcessFileContentUsingDecoder(*decoder, fileContent.get(), fileSize);
		}

		// NOTE: The MediaFoundation fallback decoder only works with file paths for now 
		//		 but that should be fine as usually only ComfyData and FArc files are loaded from memory which are already in known format
		DecoderOutputData mediaFoundationOutputData = {};
		if (TryDecodeAndParseFileUsingMediaFoundation(filePath, mediaFoundationOutputData) == DecoderResult::Success)
			return ProcessDecoderOutputDataToMemorySampleProvider(mediaFoundationOutputData);

		Logger::LogErrorLine("(): No compatible IDecoder found for the input file %.*s", filePath.size(), filePath.data());
		return nullptr;
	}

	std::unique_ptr<ISampleProvider> DecoderFactory::DecodeFileContent(std::string_view fileName, const void* fileContent, size_t fileSize)
	{
		if (fileContent == nullptr || fileSize == 0)
			return nullptr;

		const auto extension = IO::Path::GetExtension(fileName);
		for (auto& decoder : availableDecoders)
		{
			if (IO::Path::DoesAnyPackedExtensionMatch(extension, decoder->GetFileExtensions()))
				return DecodeAndProcessFileContentUsingDecoder(*decoder, fileContent, fileSize);
		}

		Logger::LogErrorLine("(): No compatible IDecoder found for the input file %.*s", fileName.size(), fileName.data());
		return nullptr;
	}

	template <typename T>
	IDecoder* DecoderFactory::RegisterDecoder()
	{
		static_assert(std::is_base_of_v<IDecoder, T>, "T must inherit from IAudioDecoder");
		return availableDecoders.emplace_back(std::make_unique<T>()).get();
	}

	void DecoderFactory::RegisterAllDecoders()
	{
		availableDecoders.reserve(5);
		RegisterDecoder<FlacDecoder>();
		RegisterDecoder<HevagDecoder>();
		RegisterDecoder<Mp3Decoder>();
		RegisterDecoder<VorbisDecoder>();
		wavDecoder = RegisterDecoder<WavDecoder>();
	}

	std::unique_ptr<ISampleProvider> DecoderFactory::DecodeAndProcessFileContentUsingDecoder(IDecoder& decoder, const void* fileContent, size_t fileSize)
	{
		DecoderOutputData outputData = {};
		if (decoder.DecodeParseAudio(fileContent, fileSize, outputData) == DecoderResult::Failure)
			return nullptr;

		return ProcessDecoderOutputDataToMemorySampleProvider(outputData);
	}

	std::unique_ptr<ISampleProvider> DecoderFactory::ProcessDecoderOutputDataToMemorySampleProvider(DecoderOutputData& outputData)
	{
		// TODO: Implement high quality resampling using MediaFoundation
		if (outputData.SampleRate != AudioEngine::OutputSampleRate)
			Resample<i16>(outputData.SampleData, outputData.SampleCount, outputData.SampleRate, AudioEngine::OutputSampleRate, outputData.ChannelCount);

		auto outSampleProvider = std::make_unique<MemorySampleProvider>();
		outSampleProvider->channelCount = outputData.ChannelCount;
		outSampleProvider->sampleRate = outputData.SampleRate;
		outSampleProvider->sampleCount = outputData.SampleCount;
		outSampleProvider->sampleData = std::move(outputData.SampleData);
		return outSampleProvider;
	}

	DecoderResult DecoderFactory::TryDecodeAndParseFileUsingMediaFoundation(std::string_view filePath, DecoderOutputData& outputData)
	{
        return DecoderResult::Failure;
    }
}
