#ifndef SUB_TRACK_FILTER_H_
#define SUB_TRACK_FILTER_H_

#include "abstractreader.h"

class SubTrackFilter
{
   public:
    SubTrackFilter(const int pid) : m_srcPID(pid) {}
    virtual ~SubTrackFilter() = default;

    static int pidToSubPid(const int pid, const int subPid) { return (pid << 16) + subPid; }
    static bool isSubTrack(const int pid) { return pid >= 65536; }

    bool isSupportedTrack(const int pid) const { return m_srcPID == pid; }
    virtual int demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket) = 0;

   protected:
    int m_srcPID;
};

#endif  // SUB_TRACK_FILTER_H_
