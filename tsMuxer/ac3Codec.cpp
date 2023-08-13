#include "ac3Codec.h"

#include <sstream>

#include "avCodecs.h"
#include "bitStream.h"
#include "vod_common.h"

#define max(a, b) ((a) > (b) ? (a) : (b))
static constexpr int NB_BLOCKS = 6;  // number of PCM blocks inside an AC3 frame
static constexpr int AC3_FRAME_SIZE = NB_BLOCKS * 256;

bool isSyncWord(const uint8_t *buff) { return buff[0] == 0x0B && buff[1] == 0x77; }

bool isHDSyncWord(const uint8_t *buff) { return buff[0] == 0xf8 && buff[1] == 0x72 && buff[2] == 0x6f; }

static constexpr uint8_t eac3_blocks[4] = {1, 2, 3, 6};

static constexpr uint8_t ff_ac3_channels[8] = {2, 1, 2, 3, 3, 4, 4, 5};

// possible frequencies
static constexpr uint16_t ff_ac3_freqs[3] = {48000, 44100, 32000};

// possible bitrates
static constexpr uint16_t ff_ac3_bitratetab[19] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                                   192, 224, 256, 320, 384, 448, 512, 576, 640};

static constexpr uint16_t ff_ac3_frame_sizes[38][3] = {
    {64, 69, 96},       {64, 70, 96},       {80, 87, 120},      {80, 88, 120},      {96, 104, 144},
    {96, 105, 144},     {112, 121, 168},    {112, 122, 168},    {128, 139, 192},    {128, 140, 192},
    {160, 174, 240},    {160, 175, 240},    {192, 208, 288},    {192, 209, 288},    {224, 243, 336},
    {224, 244, 336},    {256, 278, 384},    {256, 279, 384},    {320, 348, 480},    {320, 349, 480},
    {384, 417, 576},    {384, 418, 576},    {448, 487, 672},    {448, 488, 672},    {512, 557, 768},
    {512, 558, 768},    {640, 696, 960},    {640, 697, 960},    {768, 835, 1152},   {768, 836, 1152},
    {896, 975, 1344},   {896, 976, 1344},   {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
    {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920}};

static const uint16_t ctx[256] = {
    0x0000, 0x0580, 0x0F80, 0x0A00, 0x1B80, 0x1E00, 0x1400, 0x1180, 0x3380, 0x3600, 0x3C00, 0x3980, 0x2800, 0x2D80,
    0x2780, 0x2200, 0x6380, 0x6600, 0x6C00, 0x6980, 0x7800, 0x7D80, 0x7780, 0x7200, 0x5000, 0x5580, 0x5F80, 0x5A00,
    0x4B80, 0x4E00, 0x4400, 0x4180, 0xC380, 0xC600, 0xCC00, 0xC980, 0xD800, 0xDD80, 0xD780, 0xD200, 0xF000, 0xF580,
    0xFF80, 0xFA00, 0xEB80, 0xEE00, 0xE400, 0xE180, 0xA000, 0xA580, 0xAF80, 0xAA00, 0xBB80, 0xBE00, 0xB400, 0xB180,
    0x9380, 0x9600, 0x9C00, 0x9980, 0x8800, 0x8D80, 0x8780, 0x8200, 0x8381, 0x8601, 0x8C01, 0x8981, 0x9801, 0x9D81,
    0x9781, 0x9201, 0xB001, 0xB581, 0xBF81, 0xBA01, 0xAB81, 0xAE01, 0xA401, 0xA181, 0xE001, 0xE581, 0xEF81, 0xEA01,
    0xFB81, 0xFE01, 0xF401, 0xF181, 0xD381, 0xD601, 0xDC01, 0xD981, 0xC801, 0xCD81, 0xC781, 0xC201, 0x4001, 0x4581,
    0x4F81, 0x4A01, 0x5B81, 0x5E01, 0x5401, 0x5181, 0x7381, 0x7601, 0x7C01, 0x7981, 0x6801, 0x6D81, 0x6781, 0x6201,
    0x2381, 0x2601, 0x2C01, 0x2981, 0x3801, 0x3D81, 0x3781, 0x3201, 0x1001, 0x1581, 0x1F81, 0x1A01, 0x0B81, 0x0E01,
    0x0401, 0x0181, 0x0383, 0x0603, 0x0C03, 0x0983, 0x1803, 0x1D83, 0x1783, 0x1203, 0x3003, 0x3583, 0x3F83, 0x3A03,
    0x2B83, 0x2E03, 0x2403, 0x2183, 0x6003, 0x6583, 0x6F83, 0x6A03, 0x7B83, 0x7E03, 0x7403, 0x7183, 0x5383, 0x5603,
    0x5C03, 0x5983, 0x4803, 0x4D83, 0x4783, 0x4203, 0xC003, 0xC583, 0xCF83, 0xCA03, 0xDB83, 0xDE03, 0xD403, 0xD183,
    0xF383, 0xF603, 0xFC03, 0xF983, 0xE803, 0xED83, 0xE783, 0xE203, 0xA383, 0xA603, 0xAC03, 0xA983, 0xB803, 0xBD83,
    0xB783, 0xB203, 0x9003, 0x9583, 0x9F83, 0x9A03, 0x8B83, 0x8E03, 0x8403, 0x8183, 0x8002, 0x8582, 0x8F82, 0x8A02,
    0x9B82, 0x9E02, 0x9402, 0x9182, 0xB382, 0xB602, 0xBC02, 0xB982, 0xA802, 0xAD82, 0xA782, 0xA202, 0xE382, 0xE602,
    0xEC02, 0xE982, 0xF802, 0xFD82, 0xF782, 0xF202, 0xD002, 0xD582, 0xDF82, 0xDA02, 0xCB82, 0xCE02, 0xC402, 0xC182,
    0x4382, 0x4602, 0x4C02, 0x4982, 0x5802, 0x5D82, 0x5782, 0x5202, 0x7002, 0x7582, 0x7F82, 0x7A02, 0x6B82, 0x6E02,
    0x6402, 0x6182, 0x2002, 0x2582, 0x2F82, 0x2A02, 0x3B82, 0x3E02, 0x3402, 0x3182, 0x1382, 0x1602, 0x1C02, 0x1982,
    0x0802, 0x0D82, 0x0782, 0x0202};

static constexpr int AC3_ACMOD_MONO = 1;
static constexpr int AC3_ACMOD_STEREO = 2;

const CodecInfo &AC3Codec::getCodecInfo()
{
    if (m_true_hd_mode)
        return trueHDCodecInfo;
    if (isEAC3())
        return eac3CodecInfo;
    return ac3CodecInfo;
}

// returns true if ok, or false if error
bool AC3Codec::crc32(const uint8_t *buf, const int length)
{
    const uint8_t *end = buf + length;

    int crc = 0;
    while (buf < end) crc = ctx[static_cast<uint8_t>(crc) ^ *buf++] ^ (crc >> 8);

    // the last word of the frame is crc2 = crc for the whole frame except sync_byte
    if (crc != buf[0] + (buf[1] << 8))
        return false;

    return true;
}

// returns NO_ERROR, or type of error
AC3Codec::AC3ParseError AC3Codec::parseHeader(uint8_t *buf, const uint8_t *end)
{
    if (*buf++ != 0x0B || *buf++ != 0x77)
        return AC3ParseError::SYNC;

    // read ahead to bsid to make sure this is AC-3, not E-AC-3
    const uint8_t id = buf[3] >> 3;
    if (id > 16)
        return AC3ParseError::BSID;

    m_bsid = id;

    // ---------------------------------- EAC3 ------------------------------------------
    if (m_bsid > 10)
    {
        m_frame_size = static_cast<uint16_t>((((buf[0] & 0x07) << 8 | buf[1]) + 1) << 1);
        if (m_frame_size < AC3_HEADER_SIZE)
            return AC3ParseError::FRAME_SIZE;  // invalid header size

        if (end < m_frame_size + buf)
            return AC3ParseError::NOT_ENOUGH_BUFFER;

        if (!crc32(buf, m_frame_size - 4))
            return AC3ParseError::CRC2;

        uint8_t bsmod = 0, dsurmod = 0, numblkscod, mixdef = 0;
        int extpgmscle, paninfoe;
        int pgmscle = extpgmscle = paninfoe = 0;

        BitStreamReader gbc{};
        gbc.setBuffer(buf, end);

        m_strmtyp = gbc.getBits<uint8_t>(2);
        if (m_strmtyp == 3)
            return AC3ParseError::SYNC;  // invalid stream type

        gbc.skipBits(14);  // substreamid, frmsize

        const auto fscod = gbc.getBits<uint8_t>(2);

        if (fscod == 3)
        {
            const auto m_fscod2 = gbc.getBits<uint8_t>(2);
            if (m_fscod2 == 3)
                return AC3ParseError::SYNC;

            numblkscod = 3;
            m_sample_rate = ff_ac3_freqs[m_fscod2] / 2;
        }
        else
        {
            numblkscod = gbc.getBits<uint8_t>(2);
            m_sample_rate = ff_ac3_freqs[fscod];
        }
        const int number_of_blocks_per_syncframe =
            numblkscod == 0 ? 1 : (numblkscod == 1 ? 2 : (numblkscod == 2 ? 3 : 6));
        const auto acmod = gbc.getBits<uint8_t>(3);
        const auto lfeon = gbc.getBits<uint8_t>(1);

        m_samples = eac3_blocks[numblkscod] << 8;
        m_bit_rateExt = m_frame_size * m_sample_rate * 8 / m_samples;

        gbc.skipBits(5);  // skip bsid, already got it
        for (int i = 0; i < (acmod ? 1 : 2); i++)
        {
            gbc.skipBits(5);  // skip dialog normalization
            if (gbc.getBit())
                gbc.skipBits(8);  // skip Compression gain word
        }

        if (m_strmtyp == 1)  // dependent EAC3 frame
        {
            if (gbc.getBit())
            {
                const auto chanmap = gbc.getBits<int16_t>(16);
                if (chanmap & 0x7fe0)  // mask standard 5.1 channels
                    m_extChannelsExists = true;
            }
        }
        if (gbc.getBit())  // mixing metadata
        {
            if (acmod > 2)
                gbc.skipBits(2);  // dmixmod
            if ((acmod & 1) && (acmod > 0x2))
                gbc.skipBits(6);  // ltrtcmixlev, lorocmixlev
            if (acmod & 4)
                gbc.skipBits(6);  // ltrtsurmixlev, lorosurmixlev
            if (lfeon && gbc.getBit())
                gbc.skipBits(5);  // lfemixlevcod
            if (m_strmtyp == 0)   // independent EAC3 frame
            {
                pgmscle = gbc.getBit();
                if (pgmscle)
                    gbc.skipBits(6);  // pgmscl
                if (acmod == 0 && gbc.getBit())
                    gbc.skipBits(6);  // pgmscl2
                extpgmscle = gbc.getBit();
                if (extpgmscle)
                    gbc.skipBits(6);  // extpgmscl
                mixdef = gbc.getBits<uint8_t>(2);
                if (mixdef == 1)
                    gbc.skipBits(5);  // premixcmpsel, drcsrc, premixcmpscl
                else if (mixdef == 2)
                    gbc.skipBits(12);  // mixdata
                else if (mixdef == 3)
                {
                    const auto mixdeflen = gbc.getBits<uint8_t>(5);  //
                    if (gbc.getBit())                      // mixdata2e
                    {
                        gbc.skipBits(5);  // premixcmpsel, drcsrc, premixcmpscl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // extpgmlscl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // extpgmcscl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // extpgmrscl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // extpgmlscl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // extpgmrsscl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // extpgmlfescl
                        if (gbc.getBit())
                            gbc.skipBits(4);  // dmixscl
                        if (gbc.getBit())
                        {
                            if (gbc.getBit())
                                gbc.skipBits(4);  // extpgmaux1scl
                            if (gbc.getBit())
                                gbc.skipBits(4);  // extpgmaux2scl
                        }
                    }
                    if (gbc.getBit())  // mixdata3e
                    {
                        gbc.skipBits(5);  // spchdat
                        if (gbc.getBit())
                        {
                            gbc.skipBits(7);  // spchdat1, spchan1att
                            if (gbc.getBit())
                                gbc.skipBits(7);  // spchdat2, spchan2att
                        }
                    }
                    for (int i = 0; i < mixdeflen; i++) gbc.skipBits(8);  // mixdata
                    for (int i = 0; i < 7; i++)                           // mixdatafill
                        if (!gbc.showBits(1))
                            gbc.skipBit();
                        else
                            break;
                }
                if (acmod < 2)
                {
                    paninfoe = gbc.getBit();
                    if (paninfoe)
                        gbc.skipBits(14);  // panmean, paninfo
                    if (acmod == 0x0 && gbc.getBit())
                        gbc.skipBits(14);  // panmean2, paninfo2
                }
                if (gbc.getBit())
                {
                    if (numblkscod == 0)
                        gbc.skipBits(5);  // blkmixcfginfo[0]
                    else
                        for (int blk = 0; blk < number_of_blocks_per_syncframe; blk++)
                            if (gbc.getBit())
                                gbc.skipBits(5);  // blkmixcfginfo[blk]
                }
            }
        }
        if (gbc.getBit())
        {
            bsmod = gbc.getBits<uint8_t>(3);
            gbc.skipBits(2);  // copyrightb, origbs
            if (acmod == 2)
                dsurmod = gbc.getBits<uint8_t>(2);
        }
        m_mixinfoexists = pgmscle || extpgmscle || mixdef > 0 || paninfoe;

        if (m_channels == 0)  // no AC3 interleave
        {
            m_acmod = acmod;
            m_lfeon = lfeon;
            m_bsmod = bsmod;
            m_fscod = fscod;
            m_dsurmod = dsurmod;
            m_channels = static_cast<uint8_t>(ff_ac3_channels[acmod] + lfeon);
        }
    }
    // ---------------------------------- AC3 ------------------------------------------
    else
    {
        m_fscod = buf[2] >> 6;
        if (m_fscod == 3)
            return AC3ParseError::SAMPLE_RATE;

        m_frmsizecod = buf[2] & 0x3f;
        if (m_frmsizecod > 37)
            return AC3ParseError::FRAME_SIZE;

        m_frame_size = ff_ac3_frame_sizes[m_frmsizecod][m_fscod] << 1;
        if (end + 2 < m_frame_size + buf)
            return AC3ParseError::NOT_ENOUGH_BUFFER;

        if (!crc32(buf, m_frame_size - 4))
            return AC3ParseError::CRC2;

        BitStreamReader gbc{};
        gbc.setBuffer(buf + 3, end);

        m_bsidBase = m_bsid;  // id except AC3+ frames
        m_strmtyp = 2;
        m_samples = AC3_FRAME_SIZE;

        gbc.skipBits(5);  // skip bsid, already got it
        m_bsmod = gbc.getBits<uint8_t>(3);
        m_acmod = gbc.getBits<uint8_t>(3);
        if ((m_acmod & 1) && m_acmod != AC3_ACMOD_MONO)  // 3 front channels
            gbc.skipBits(2);                             // m_cmixlev
        if (m_acmod & 4)                                 // surround channel exists
            gbc.skipBits(2);                             // m_surmixlev
        if (m_acmod == AC3_ACMOD_STEREO)
            m_dsurmod = gbc.getBits<uint8_t>(2);
        m_lfeon = gbc.getBits<uint8_t>(1);

        m_halfratecod = max(m_bsid, 8) - 8;
        m_sample_rate = ff_ac3_freqs[m_fscod] >> m_halfratecod;
        m_bit_rate = (ff_ac3_bitratetab[m_frmsizecod >> 1] * 1000) >> m_halfratecod;
        m_channels = static_cast<uint8_t>(ff_ac3_channels[m_acmod] + m_lfeon);
    }
    return AC3ParseError::NO_ERROR2;
}

// returns frame length, or zero (error), or NOT_ENOUGH_BUFFER
int AC3Codec::decodeFrame(uint8_t *buf, uint8_t *end, int &skipBytes)
{
    try
    {
        int rez = 0;

        if (end - buf < 2)
            return NOT_ENOUGH_BUFFER;
        if (m_state != AC3State::stateDecodeAC3 && buf[0] == 0x0B && buf[1] == 0x77)
        {
            if (testDecodeTestFrame(buf, end))
                m_state = AC3State::stateDecodeAC3;
        }

        if (m_state == AC3State::stateDecodeAC3)
        {
            skipBytes = 0;
            const AC3ParseError err = parseHeader(buf, end);

            if (err == AC3ParseError::NOT_ENOUGH_BUFFER)
                return NOT_ENOUGH_BUFFER;

            if (err != AC3ParseError::NO_ERROR2)
                return 0;  // parse error

            m_frameDuration = (INTERNAL_PTS_FREQ * m_samples) / m_sample_rate;
            rez = m_frame_size;
        }

        if (getTestMode() && !m_true_hd_mode)
        {
            uint8_t *trueHDData = buf + rez;
            if (end - trueHDData < 8)
                return NOT_ENOUGH_BUFFER;
            if (!isSyncWord(trueHDData) && isHDSyncWord(trueHDData + 4))
            {
                if (end - trueHDData < 21)
                    return NOT_ENOUGH_BUFFER;
                m_true_hd_mode = mlp.decodeFrame(trueHDData, trueHDData + 21);
            }
        }

        if ((m_true_hd_mode))  // omit AC3+
        {
            uint8_t *trueHDData = buf + rez;
            if (end - trueHDData < 7)
                return NOT_ENOUGH_BUFFER;
            if (m_state == AC3State::stateDecodeAC3)
            {
                // check if it is a real HD frame

                if (trueHDData[0] != 0x0B || trueHDData[1] != 0x77 || !testDecodeTestFrame(trueHDData, end))
                {
                    m_waitMoreData = true;
                    m_state = AC3State::stateDecodeTrueHDFirst;
                }
                return rez;
            }
            m_state = AC3State::stateDecodeTrueHD;
            int trueHDFrameLen = (trueHDData[0] & 0x0f) << 8;
            trueHDFrameLen += trueHDData[1];
            trueHDFrameLen *= 2;
            if (end - trueHDData < static_cast<int64_t>(trueHDFrameLen) + 7)
                return NOT_ENOUGH_BUFFER;
            if (!m_true_hd_mode)
            {
                m_true_hd_mode = mlp.decodeFrame(trueHDData, trueHDData + trueHDFrameLen);
            }
            uint8_t *nextFrame = trueHDData + trueHDFrameLen;

            if (nextFrame[0] == 0x0B && nextFrame[1] == 0x77 && testDecodeTestFrame(nextFrame, end))
                m_waitMoreData = false;

            if (m_downconvertToAC3)
            {
                skipBytes = trueHDFrameLen;
                return 0;
            }
            return trueHDFrameLen;
        }
        if (m_downconvertToAC3 && m_bsid > 10)
        {
            skipBytes = rez;  // skip E-AC3 frame
            return 0;
        }
        return rez;
    }
    catch (BitStreamException &)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

AC3Codec::AC3ParseError AC3Codec::testParseHeader(uint8_t *buf, uint8_t *end) const
{
    BitStreamReader gbc{};
    gbc.setBuffer(buf, buf + 7);

    const auto test_sync_word = gbc.getBits<int16_t>(16);
    if (test_sync_word != 0x0B77)
        return AC3ParseError::SYNC;

    // read ahead to bsid to make sure this is AC-3, not E-AC-3
    const int test_bsid = gbc.showBits(29) & 0x1F;
    /*
    if (test_bsid != m_bsid)
    return AC3_PARSE_ERROR_SYNC;
    */

    if (test_bsid > 16)
    {
        return AC3ParseError::SYNC;  // invalid stream type
    }
    if (m_bsid > 10)
    {
        return AC3ParseError::SYNC;  // doesn't used for EAC3
    }
    gbc.skipBits(16);  // test_crc1
    const auto test_fscod = gbc.getBits<uint8_t>(2);
    if (test_fscod == 3)
        return AC3ParseError::SAMPLE_RATE;

    const auto test_frmsizecod = gbc.getBits<uint8_t>(6);
    if (test_frmsizecod > 37)
        return AC3ParseError::FRAME_SIZE;

    gbc.skipBits(5);  // skip bsid, already got it

    const auto test_bsmod = gbc.getBits<uint8_t>(3);
    const auto test_acmod = gbc.getBits<uint8_t>(3);

    if (test_fscod != m_fscod || /*(test_frmsizecod>>1) != (m_frmsizecod>>1) ||*/
        test_bsmod != m_bsmod /*|| test_acmod != m_acmod*/)
        return AC3ParseError::SYNC;

    if ((test_acmod & 1) && test_acmod != AC3_ACMOD_MONO)
        gbc.skipBits(2);  // test_cmixlev

    if (m_acmod & 4)
        gbc.skipBits(2);  // test_surmixlev

    if (m_acmod == AC3_ACMOD_STEREO)
    {
        const auto test_dsurmod = gbc.getBits<uint8_t>(2);
        if (test_dsurmod != m_dsurmod)
            return AC3ParseError::SYNC;
    }
    const int test_lfeon = gbc.getBit();

    if (test_lfeon != m_lfeon)
        return AC3ParseError::SYNC;

    const int test_halfratecod = max(test_bsid, 8) - 8;
    const int test_sample_rate = ff_ac3_freqs[test_fscod] >> test_halfratecod;
    const int test_bit_rate = (ff_ac3_bitratetab[test_frmsizecod >> 1] * 1000) >> test_halfratecod;
    const int test_channels = ff_ac3_channels[test_acmod] + test_lfeon;
    const int test_frame_size = ff_ac3_frame_sizes[test_frmsizecod][test_fscod] * 2;
    if (test_halfratecod != m_halfratecod || test_sample_rate != m_sample_rate || test_bit_rate != m_bit_rate ||
        test_channels != m_channels || test_frame_size != m_frame_size)
        return AC3ParseError::SYNC;
    return AC3ParseError::NO_ERROR2;
}

bool AC3Codec::testDecodeTestFrame(uint8_t *buf, uint8_t *end) const
{
    return testParseHeader(buf, end) == AC3ParseError::NO_ERROR2;
}

uint64_t AC3Codec::getFrameDuration() const
{
    // EAC3 dependent frame : wait for next independent frame
    if (!m_bit_rate && m_strmtyp == 1)
        return 0;

    // AC3 frame in AC3/EAC3 stream: wait for EAC3 frame
    if (m_bit_rateExt && m_bsid <= 10)
        return 0;

    // Interleaved AC3/TrueHD: wait for end of True HD frame
    if (m_waitMoreData)
        return 0;

    // OK to increment PTS
    return m_frameDuration;
}

const std::string AC3Codec::getStreamInfo()
{
    std::ostringstream str;
    std::string hd_type;
    if (mlp.m_subType == MlpSubType::stTRUEHD)
        hd_type = "TRUE-HD";
    else
        hd_type = (mlp.m_subType == MlpSubType::stMLP) ? "MLP" : "UNKNOWN";

    if (m_true_hd_mode)
    {
        if (isEAC3())
            str << "E-";
        str << "AC3 core + ";
        str << hd_type;
        if (mlp.m_substreams == 4)
            str << " + ATMOS";
        str << ". ";

        str << "Peak bitrate: " << mlp.m_bitrate / 1000 << "Kbps (core " << m_bit_rate / 1000 << "Kbps) ";
        str << "Sample Rate: " << mlp.m_samplerate / 1000 << "KHz ";
        if (m_sample_rate != mlp.m_samplerate)
            str << " (core " << m_sample_rate / 1000 << "Khz) ";
    }
    else
    {
        str << "Bitrate: " << (m_bit_rate + m_bit_rateExt) / 1000 << "Kbps ";
        if (m_bit_rateExt)
            str << "(core " << m_bit_rate / 1000 << "Kbps) ";
        str << "Sample Rate: " << m_sample_rate / 1000 << "KHz ";
    }

    str << "Channels: ";
    int channels = m_channels;
    if (m_extChannelsExists)
        channels += 2;
    if (mlp.m_channels)
        channels = mlp.m_channels;

    if (m_lfeon)
        str << channels - 1 << ".1";
    else
        str << channels;
    return str.str();
}

uint8_t *AC3Codec::findFrame(uint8_t *buffer, const uint8_t *end)
{
    if (buffer == nullptr)
        return nullptr;
    uint8_t *curBuf = buffer;
    while (curBuf < end - 1)
    {
        if (*curBuf == 0x0B && curBuf[1] == 0x77)
            return curBuf;
        curBuf++;
    }
    return nullptr;
}
