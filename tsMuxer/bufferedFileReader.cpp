#include "bufferedFileReader.h"

#include <fs/systemlog.h>

#include <iostream>

#include "vodCoreException.h"
#include "vod_common.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace std;

bool FileReaderData::openStream()
{
    base_class::openStream();

    const bool rez = m_file.open(m_streamName.c_str(), File::ofRead);

    if (!rez)
    {
#ifdef _WIN32
        LPVOID msgBuf = nullptr;
        const DWORD dw = GetLastError();

        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, dw,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&msgBuf), 0, nullptr);

        string str(static_cast<char*>(msgBuf));
        LTRACE(LT_ERROR, 0, str);
#endif
    }

    return rez;
}

BufferedFileReader::BufferedFileReader(const uint32_t blockSize, const uint32_t allocSize,
                                       const uint32_t prereadThreshold)
    : BufferedReader(blockSize, allocSize, prereadThreshold)
{
}

bool BufferedFileReader::openStream(const uint32_t readerID, const char* streamName, int pid,
                                    const CodecInfo* codecInfo)
{
    const auto data = static_cast<FileReaderData*>(getReader(readerID));

    if (data == nullptr)
    {
        LTRACE(LT_ERROR, 0, "Unknown readerID " << readerID);
        return false;
    }
    data->m_firstBlock = true;
    data->m_lastBlock = false;
    data->m_streamName = streamName;
    data->m_fileHeaderSize = 0;
    data->closeStream();
    if (!data->openStream())
    {
        return false;
    }
    return true;
}
bool BufferedFileReader::gotoByte(const uint32_t readerID, const uint64_t seekDist)
{
    const auto data = static_cast<FileReaderData*>(getReader(readerID));
    if (data)
    {
        data->m_blockSize = m_blockSize - static_cast<uint32_t>(seekDist % static_cast<uint64_t>(m_blockSize));
        const uint64_t seekRez = data->m_file.seek(seekDist + data->m_fileHeaderSize, File::SeekMethod::smBegin);
        const bool rez = seekRez != static_cast<uint64_t>(-1);
        if (rez)
        {
            data->m_eof = false;
            data->m_notified = false;
        }
        return rez;
    }
    else
        return false;
}
