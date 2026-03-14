// WinDirStat - Directory Statistics
// Copyright © WinDirStat Team
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "pch.h"
#include "FinderNtfs.h"

enum ATTRIBUTE_TYPE_CODE : ULONG
{
    AttributeStandardInformation = 0x10,
    AttributeFileName = 0x30,
    AttributeData = 0x80,
    AttributeReparsePoint = 0xC0,
    AttributeEnd = 0xFFFFFFFF,
};

using FILE_RECORD = struct FILE_RECORD
{
    ULONG Signature;
    USHORT UsaOffset;
    USHORT UsaCount;
    ULONGLONG Lsn;
    USHORT SequenceNumber;
    USHORT LinkCount;
    USHORT FirstAttributeOffset;
    USHORT Flags;
    ULONG FirstFreeByte;
    ULONG BytesAvailable;
    ULONGLONG BaseFileRecordNumber : 48;
    ULONGLONG BaseFileRecordSequence : 16;
    USHORT NextAttributeNumber;
    USHORT SegmentNumberHighPart;
    ULONG SegmentNumberLowPart;

    constexpr ULONGLONG SegmentNumber() const noexcept
    {
        return static_cast<ULONGLONG>(SegmentNumberHighPart) << 32ul | SegmentNumberLowPart;
    }

    constexpr bool IsValid() const noexcept
    {
        return Signature == 0x454C4946; // 'FILE'
    }
    constexpr bool IsInUse() const noexcept
    {
        return Flags & 0x0001;
    }

    constexpr bool IsDirectory() const noexcept
    {
        return Flags & 0x0002;
    }
};

using ATTRIBUTE_RECORD = struct ATTRIBUTE_RECORD
{
    ATTRIBUTE_TYPE_CODE TypeCode;
    ULONG RecordLength;
    UCHAR FormCode;
    UCHAR NameLength;
    USHORT NameOffset;
    USHORT Flags;
    USHORT Instance;

    union
    {
        struct
        {
            ULONG ValueLength;
            USHORT ValueOffset;
            UCHAR Reserved[2];
        } Resident;

        struct
        {
            LONGLONG LowestVcn;
            LONGLONG HighestVcn;
            USHORT DataRunOffset;
            USHORT CompressionSize;
            UCHAR Padding[4];
            ULONGLONG AllocatedLength;
            ULONGLONG FileSize;
            ULONGLONG ValidDataLength;
            ULONGLONG Compressed;
        } Nonresident;
    } Form;

    constexpr bool IsNonResident() const noexcept
    {
        return FormCode & 0x0001;
    }

    constexpr bool IsCompressed() const noexcept
    {
        return Flags & 0x0001;
    }

    constexpr bool IsSparse() const noexcept
    {
        return Flags & 0x8000;
    }

    ATTRIBUTE_RECORD* next() const noexcept
    {
        return ByteOffset<ATTRIBUTE_RECORD>(const_cast<ATTRIBUTE_RECORD*>(this), RecordLength);
    }

    static constexpr std::pair<ATTRIBUTE_RECORD*, ATTRIBUTE_RECORD*> bounds(FILE_RECORD* FileRecord, auto TotalLength) noexcept
    {
        return {
            ByteOffset<ATTRIBUTE_RECORD>(FileRecord, FileRecord->FirstAttributeOffset),
            ByteOffset<ATTRIBUTE_RECORD>(FileRecord, FileRecord->FirstAttributeOffset + TotalLength)
        };
    }
};

using FILE_NAME = struct FILE_NAME
{
    ULONGLONG ParentDirectory : 48;
    ULONGLONG ParentSequence : 16;
    LONGLONG CreationTime;
    LONGLONG LastModificationTime;
    LONGLONG MftChangeTime;
    LONGLONG LastAccessTime;
    LONGLONG AllocatedLength;
    LONGLONG FileSize;
    ULONG FileAttributes;
    USHORT PackedEaSize;
    USHORT Reserved;
    UCHAR FileNameLength;
    UCHAR Flags;
    WCHAR FileName[1];

    constexpr bool IsShortNameRecord() const noexcept
    {
        return Flags == 0x02;
    }
};

using STANDARD_INFORMATION = struct STANDARD_INFORMATION
{
    FILETIME CreationTime;
    FILETIME LastModificationTime;
    FILETIME MftChangeTime;
    FILETIME LastAccessTime;
    ULONG FileAttributes;
};

bool FinderNtfs::FindNext()
{
    if (m_recordIterator == m_recordIteratorEnd) return false;
    m_index = m_recordIterator->BaseRecord;
    const auto it = m_master->m_baseFileRecordMap.find(m_index);
    if (it == m_master->m_baseFileRecordMap.end()) return false;
    m_currentRecord = &it->second;
    m_currentRecordName = &(*m_recordIterator);
    ++m_recordIterator;

    return true;
}

bool FinderNtfs::FindFile(const std::wstring& strFolder, ULONGLONG index, const DWORD attr)
{
    m_base = strFolder;
    const auto result = m_master->m_parentToChildMap.find(index);
    if (result == m_master->m_parentToChildMap.end()) return false;
    m_recordIteratorEnd = result->second.end();
    m_recordIterator = result->second.begin();
    return FindNext();
}

DWORD FinderNtfs::GetAttributes() const
{
    return m_currentRecord->Attributes;
}

ULONGLONG FinderNtfs::GetIndex() const
{
    return m_currentRecordName->BaseRecord;
}

DWORD FinderNtfs::GetReparseTag() const
{
    return m_currentRecord->ReparsePointTag;
}

std::wstring FinderNtfs::GetFileName() const
{
    return m_currentRecordName->FileName;
}

ULONGLONG FinderNtfs::GetFileSizePhysical() const
{
    return m_currentRecord->PhysicalSize;
}

ULONGLONG FinderNtfs::GetFileSizeLogical() const
{
    return m_currentRecord->LogicalSize;
}

FILETIME FinderNtfs::GetLastWriteTime() const
{
    return m_currentRecord->LastModifiedTime;
}

std::wstring FinderNtfs::GetFilePath() const
{
    // Get full path to folder or file
    std::wstring path = (m_base.back() == L'\\') ?
        (m_base + GetFileName()) :
        (m_base + L"\\" + GetFileName());

    // Strip special dos chars
    if (path.starts_with(s_dosUNCPath)) return L"\\\\" + path.substr(s_dosUNCPath.length());
    if (path.starts_with(s_dosPath)) return path.substr(s_dosPath.length());
    return path;
}

bool FinderNtfs::IsReserved() const
{
    return m_index < FinderNtfsContext::NtfsReservedMax;
}
