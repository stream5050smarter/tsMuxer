#ifndef MLP_CODEC_H_
#define MLP_CODEC_H_

#include <types/types.h>

static constexpr int MLP_HEADER_LEN = 7;

enum class MlpSubType
{
    stUnknown,
    stTRUEHD,
    stMLP
};

class MLPCodec
{
   public:
    MLPCodec()
        : m_channels(0), m_samples(0), m_samplerate(0), m_bitrate(0), m_substreams(0), m_subType(MlpSubType::stUnknown)
    {
    }
    static uint8_t* findFrame(uint8_t* buffer, const uint8_t* end);
    static int getFrameSize(const uint8_t* buffer);
    bool decodeFrame(uint8_t* buffer, uint8_t* end);
    bool isMinorSync(const uint8_t* buffer, uint8_t* end) const;
    uint64_t getFrameDuration() const;
    static int mlp_samplerate(int ratebits);

    uint8_t m_channels;
    int m_samples;
    int m_samplerate;  // Sample rate of first substream
    int m_bitrate;     // Peak bitrate for VBR, actual bitrate for CBR
    uint8_t m_substreams;
    MlpSubType m_subType;
};

#endif
