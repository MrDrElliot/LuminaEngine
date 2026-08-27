#pragma once

#include "AudioTypes.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "WaveDecoder.h"

namespace Lumina
{
	class FAudioGraphInstance;
	class FProceduralAudioStream;

	/** Anything the mixer pulls interleaved float samples from, at the source's own rate and layout. */
	class RUNTIME_API IAudioSource
	{
	public:

		virtual ~IAudioSource() = default;

		// Fills up to NumFrames and returns how many carried real data; a short read means it ran out.
		virtual uint32 Pull(float* Out, uint32 NumFrames) = 0;

		virtual uint32 GetSampleRate() const = 0;
		virtual uint32 GetChannelCount() const = 0;

		// True once no further frame will ever arrive; a live stream never reports this.
		virtual bool IsAtEnd() const = 0;

		virtual uint64 GetCursor() const { return 0; }

		virtual void Seek(uint64 Frame) { (void)Frame; }
		virtual void SetLooping(bool bInLooping) { (void)bInLooping; }

		/** False for generated sources, which have no timeline to scrub and are never auto-collected. */
		virtual bool IsSeekable() const { return false; }
	};

	/** A wave clip decoded on demand straight out of the encoded bytes, so seeking costs nothing. */
	class RUNTIME_API FWaveAudioSource final : public IAudioSource
	{
	public:

		// Asset-backed. The shared bytes are held for the voice's lifetime so an unload cannot pull them away.
		bool Open(const TSharedPtr<FAudioData>& Data);

		// Loose-file playback; the source takes ownership of the bytes.
		bool Open(TVector<uint8>&& Bytes);

		uint32 Pull(float* Out, uint32 NumFrames) override;

		uint32 GetSampleRate() const override { return Reader.GetInfo().SampleRate; }
		uint32 GetChannelCount() const override { return Reader.GetInfo().NumChannels; }

		bool IsAtEnd() const override;
		uint64 GetCursor() const override { return Cursor; }

		void Seek(uint64 Frame) override;
		void SetLooping(bool bInLooping) override { bLooping = bInLooping; }
		bool IsSeekable() const override { return true; }

	private:

		bool OpenReader(const void* Data, size_t Size);

		TSharedPtr<FAudioData> Shared;
		TVector<uint8>         Owned;

		Audio::FWaveReader Reader;

		uint64 Cursor = 0;
		bool bLooping = false;
	};

	/** A live ring the game pushes samples into; an underrun reads as silence, never as an end. */
	class RUNTIME_API FProceduralAudioSource final : public IAudioSource
	{
	public:

		explicit FProceduralAudioSource(const TSharedPtr<FProceduralAudioStream>& InStream);

		uint32 Pull(float* Out, uint32 NumFrames) override;

		uint32 GetSampleRate() const override;
		uint32 GetChannelCount() const override;

		bool IsAtEnd() const override { return false; }

	private:

		TSharedPtr<FProceduralAudioStream> Stream;
	};

	/** A compiled audio graph rendered block by block; it ends when the graph raises OnFinished. */
	class RUNTIME_API FAudioGraphSource final : public IAudioSource
	{
	public:

		explicit FAudioGraphSource(const TSharedPtr<FAudioGraphInstance>& InInstance);

		uint32 Pull(float* Out, uint32 NumFrames) override;

		uint32 GetSampleRate() const override;
		uint32 GetChannelCount() const override;

		bool IsAtEnd() const override;
		uint64 GetCursor() const override;

	private:

		TSharedPtr<FAudioGraphInstance> Instance;
	};
}
