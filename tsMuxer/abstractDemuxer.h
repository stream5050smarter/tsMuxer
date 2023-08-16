#ifndef ABSTRACT_DEMUXER_H_
#define ABSTRACT_DEMUXER_H_

#include <assert.h>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include <types/types.h>

#include "avPacket.h"
#include "vod_common.h"

class SubTrackFilter;

class MemoryBlock
{
   public:
    MemoryBlock(const MemoryBlock& other)
    {
        assert(other.m_size == 0);
        m_size = 0;
    }

    MemoryBlock() : m_size(0) {}
    void reserve(const unsigned num) { m_data.resize(num); }

    void resize(const unsigned num)
    {
        m_size = num;
        if (m_data.size() < m_size)
            m_data.resize(m_size);
    }

    void grow(const size_t num)
    {
        m_size += num;
        if (m_data.size() < m_size)
        {
            m_data.resize(FFMIN(m_size * 2, m_size + 1024LL * 1024));
        }
    }

    void append(const uint8_t* data, const size_t num)
    {
        if (num > 0)
        {
            grow(num);
            memcpy(&m_data[m_size - num], data, num);
        }
    }

    [[nodiscard]] size_t size() const { return m_size; }

    uint8_t* data() { return m_data.empty() ? nullptr : m_data.data(); }

    [[nodiscard]] bool isEmpty() const { return m_size == 0; }

    void clear() { m_size = 0; }

   private:
    std::vector<uint8_t> m_data;
    size_t m_size;
};

typedef MemoryBlock StreamData;
typedef std::map<int32_t, StreamData> DemuxedData;
typedef std::set<int32_t> PIDSet;
// typedef std::map<uint32_t, std::vector<uint8_t> > DemuxedData;

// Used to automatically switch to reading the next file while the current one ends.
// This class implements file-IO, which determines the name of the next file.
class FileNameIterator
{
   public:
    virtual std::string getNextName() = 0;
    virtual ~FileNameIterator() = default;
};

struct TrackInfo
{
    int m_trackType;     // 0 - not speciffed. Autodetect is required
    std::string m_lang;  // tracl language code
    int64_t m_delay;     // auto delay for audio
    TrackInfo() : m_trackType(0), m_delay(0) {}
    TrackInfo(const int trackType, const char* lang, const int64_t delay)
        : m_trackType(trackType), m_lang(lang), m_delay(delay)
    {
    }
};

class AbstractDemuxer
{
   public:
    typedef std::map<int, SubTrackFilter*> PIDFilters;

    AbstractDemuxer()
    {
        m_fileBlockSize = DEFAULT_FILE_BLOCK_SIZE;
        m_timeOffset = 0;
    }
    virtual ~AbstractDemuxer();
    virtual void openFile(const std::string& streamName) = 0;

    virtual void readClose() = 0;
    virtual int64_t getDemuxedSize() = 0;

    virtual uint64_t getDuration() { return 0; }
    virtual void setTimeOffset(const int64_t offset) { m_timeOffset = offset; }
    virtual int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize)
    {
        discardSize = 0;
        return 0;
    }
    virtual void terminate() {}
    virtual int getLastReadRez() = 0;
    virtual void getTrackList(std::map<int32_t, TrackInfo>& trackList) {}
    virtual void setFileIterator(FileNameIterator*) {}

    virtual uint32_t getFileBlockSize() { return m_fileBlockSize; }

    virtual int64_t getTrackDelay(int32_t pid) { return 0; }
    virtual std::vector<AVChapter> getChapters() { return {}; }
    virtual double getTrackFps(uint32_t trackId) { return 0.0; }

    SubTrackFilter* getPidFilter(const int pid)
    {
        const PIDFilters::const_iterator itr = m_pidFilters.find(pid);
        return itr != m_pidFilters.end() ? itr->second : nullptr;
    }
    void setPidFilter(const int pid, SubTrackFilter* pidFilter) { m_pidFilters[pid] = pidFilter; }
    [[nodiscard]] virtual bool isPidFilterSupported() const { return false; }
    [[nodiscard]] virtual int64_t getFileDurationNano() const { return 0; }

   protected:
    int64_t m_timeOffset;
    uint32_t m_fileBlockSize;
    PIDFilters m_pidFilters;  // todo: refactor in case if several pidFilters required (>1 3D tracks)
};

#endif
