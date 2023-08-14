#ifndef MATROSKA_STREAM_READER_H_
#define MATROSKA_STREAM_READER_H_

#include "ioContextDemuxer.h"
#include "matroskaParser.h"

class MatroskaDemuxer final : public IOContextDemuxer
{
   public:
    MatroskaDemuxer(const BufferedReaderManager &readManager);
    ~MatroskaDemuxer() override { readClose(); }
    void openFile(const std::string &streamName) override;
    int readPacket(AVPacket &avPacket);  // not implemented
    void readClose() override;
    int simpleDemuxBlock(DemuxedData &demuxedData, const PIDSet &acceptedPIDs, int64_t &discardSize) override;
    void getTrackList(std::map<int32_t, TrackInfo> &trackList) override;
    std::vector<AVChapter> getChapters() override;

    bool isPidFilterSupported() const override { return true; }
    int64_t getTrackDelay(const int32_t pid) override
    {
        return (m_firstTimecode.find(pid) != m_firstTimecode.end()) ? m_firstTimecode[pid] : 0;
    }

    int64_t getFileDurationNano() const override { return fileDuration; }

   private:
    typedef Track MatroskaTrack;
    typedef IOContextTrackType MatroskaTrackType;

    typedef struct MatroskaLevel
    {
        int64_t start;
        int64_t length;
    } MatroskaLevel;

    typedef struct MatroskaDemuxIndex
    {
        uint64_t pos;  /* of the corresponding *cluster*! */
        int16_t track; /* reference to 'num' */
        uint64_t time; /* in nanoseconds */
    } MatroskaDemuxIndex;

    // ffmpeg matroska vars
    MatroskaLevel levels[EBML_MAX_DEPTH];
    std::queue<AVPacket *> packets;
    std::vector<MatroskaDemuxIndex> indexes;
    // std::vector<MatroskaDemuxLevel> levels;
    int num_levels;
    int level_up;
    int peek_id;
    bool done;
    char m_title[1024];

    uint64_t segment_start;
    uint64_t created;
    int64_t fileDuration;
    char *writing_app;
    char *muxing_app;
    uint64_t time_scale;
    std::map<int64_t, int64_t> m_firstTimecode;
    bool index_parsed;
    bool metadata_parsed;
    int num_streams;

    AVPacket *m_lastDeliveryPacket;

    uint32_t ebml_peek_id(int *levelUp);
    int ebml_read_element_id(uint32_t *id, int *levelUp);
    int ebml_read_num(int max_size, int64_t *number);
    int ebml_read_element_level_up();
    int matroska_parse_cluster();
    int ebml_read_binary(uint32_t *id, uint8_t **binary, int *size);
    int ebml_read_element_length(int64_t *length);
    int ebml_read_master(uint32_t *id);
    int ebml_read_skip();
    int ebml_read_uint(uint32_t *id, int64_t *num);
    int ebml_read_sint(uint32_t *id, int64_t *num);
    static int matroska_ebmlnum_sint(const uint8_t *data, int32_t size, int64_t *num);
    int matroska_parse_blockgroup(int64_t cluster_time);
    static int matroska_ebmlnum_uint(const uint8_t *data, int32_t size, uint64_t *num);
    int matroska_find_track_by_num(int64_t num) const;
    int matroska_parse_block(uint8_t *data, int size, int64_t pos, int64_t cluster_time, int64_t duration,
                             int is_keyframe, int is_bframe);
    static int rv_offset(const uint8_t *data, int slice, int slices);
    void matroska_queue_packet(AVPacket *pkt);
    int matroska_deliver_packet(AVPacket *&avPacket);
    int matroska_read_header();
    int ebml_read_header(char **doctype, int *version);
    int ebml_read_ascii(uint32_t *id, char **str);
    int matroska_parse_index();
    int matroska_parse_info();
    int ebml_read_date(uint32_t *id, int64_t *date);
    int ebml_read_float(uint32_t *id, double *num);

    int ebml_read_utf8(uint32_t *id, char **str);
    int matroska_parse_metadata();
    int ebml_read_seek(int64_t offset);
    int matroska_parse_tracks();
    int matroska_parse_chapters();
    int matroska_add_stream();
    static int getTrackType(const MatroskaTrack *track);

    int readTrackEncodings(MatroskaTrack *track);
    int readTrackEncoding(MatroskaTrack *track);
    int readEncodingCompression(MatroskaTrack *track);
    void decompressData(uint8_t *data, int size);

    std::map<uint64_t, AVChapter> chapters;
    MemoryBlock m_tmpBuffer;
};

#endif
