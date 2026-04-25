use std::mem::MaybeUninit;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
#[cfg(test)]
use std::{cell::RefCell, rc::Rc};
use std::{ffi::OsString, ptr};

use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, SetLastError, FILETIME, HANDLE, INVALID_HANDLE_VALUE, NO_ERROR,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FindClose, FindFirstFileW, FindNextFileW, GetCompressedFileSizeW,
    GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION, FILE_ATTRIBUTE_DIRECTORY,
    FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_REPARSE_POINT, FILE_ATTRIBUTE_SYSTEM,
    FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_READ_ATTRIBUTES,
    FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING, WIN32_FIND_DATAW,
};

const IO_REPARSE_TAG_MOUNT_POINT_VALUE: u32 = 0xA0000003;
const IO_REPARSE_TAG_SYMLINK_VALUE: u32 = 0xA000000C;

#[cfg(test)]
thread_local! {
    static TEST_DISCOVERY_OVERRIDE: RefCell<Option<Rc<dyn Fn(&[u16]) -> Result<Vec<DiscoveredEntry>, String>>>> =
        const { RefCell::new(None) };
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiscoveryEntryType {
    File,
    Directory,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveryEntry {
    pub name: String,
    pub entry_type: DiscoveryEntryType,
    pub size: Option<u64>,
    pub size_logical: Option<u64>,
    pub size_physical: Option<u64>,
    pub index: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiscoveredEntryKind {
    File,
    Directory,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveredEntry {
    pub kind: DiscoveredEntryKind,
    pub name: Vec<u16>,
    pub full_path: Vec<u16>,
    pub size_physical: u64,
    pub size_logical: u64,
    pub index: u64,
    pub last_write_time: u64,
    pub attributes: u32,
    pub reparse_tag: u32,
    pub is_reserved: bool,
    pub is_off_volume: bool,
    pub is_protected_reparse_point: bool,
}

fn widestring_without_nul(value: &[u16]) -> &[u16] {
    match value.iter().position(|ch| *ch == 0) {
        Some(index) => &value[..index],
        None => value,
    }
}

fn join_child_path(parent: &str, child: &[u16]) -> Vec<u16> {
    let parent_path = OsString::from(parent);
    let mut full_path: Vec<u16> = parent_path.as_os_str().encode_wide().collect();
    if !full_path.is_empty() && *full_path.last().unwrap() != b'\\' as u16 {
        full_path.push(b'\\' as u16);
    }
    full_path.extend_from_slice(child);
    full_path
}

fn make_search_pattern(path: &str) -> Vec<u16> {
    let path_value = OsString::from(path);
    let mut wide: Vec<u16> = path_value.as_os_str().encode_wide().collect();
    if !wide.is_empty() && *wide.last().unwrap() != b'\\' as u16 {
        wide.push(b'\\' as u16);
    }
    wide.push(b'*' as u16);
    wide.push(0);
    wide
}

fn combine_high_low(high: u32, low: u32) -> u64 {
    ((high as u64) << 32) | (low as u64)
}

fn filetime_to_u64(value: FILETIME) -> u64 {
    combine_high_low(value.dwHighDateTime, value.dwLowDateTime)
}

fn get_reparse_tag(find_data: &WIN32_FIND_DATAW) -> u32 {
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 {
        find_data.dwReserved0
    } else {
        0
    }
}

fn is_off_volume_reparse_point(reparse_tag: u32) -> bool {
    reparse_tag == IO_REPARSE_TAG_MOUNT_POINT_VALUE || reparse_tag == IO_REPARSE_TAG_SYMLINK_VALUE
}

fn is_protected_reparse_point(attributes: u32) -> bool {
    let protected = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT;
    (attributes & protected) == protected
}

fn get_logical_size(find_data: &WIN32_FIND_DATAW) -> u64 {
    combine_high_low(find_data.nFileSizeHigh, find_data.nFileSizeLow)
}

fn close_handle(handle: HANDLE) {
    if handle != INVALID_HANDLE_VALUE && !handle.is_null() {
        unsafe {
            CloseHandle(handle);
        }
    }
}

fn nul_terminated_path(path: &[u16]) -> Vec<u16> {
    let mut value = Vec::with_capacity(path.len() + 1);
    value.extend_from_slice(path);
    value.push(0);
    value
}

fn utf16_path_to_string_lossy(path: &[u16]) -> String {
    String::from_utf16_lossy(path)
}

fn try_get_file_index(full_path: &[u16], attributes: u32) -> u64 {
    let full_path = nul_terminated_path(full_path);
    let flags = FILE_FLAG_OPEN_REPARSE_POINT
        | if (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 {
            FILE_FLAG_BACKUP_SEMANTICS
        } else {
            0
        };
    let handle = unsafe {
        CreateFileW(
            full_path.as_ptr(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            ptr::null(),
            OPEN_EXISTING,
            flags,
            ptr::null_mut(),
        )
    };
    if handle == INVALID_HANDLE_VALUE {
        return 0;
    }

    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    let success = unsafe { GetFileInformationByHandle(handle, &mut info) != 0 };
    close_handle(handle);

    if success {
        combine_high_low(info.nFileIndexHigh, info.nFileIndexLow)
    } else {
        0
    }
}

fn get_physical_size(full_path: &[u16], fallback_logical_size: u64) -> u64 {
    let full_path = nul_terminated_path(full_path);
    let mut high_part = 0;
    unsafe {
        SetLastError(NO_ERROR);
    }
    let low_part = unsafe { GetCompressedFileSizeW(full_path.as_ptr(), &mut high_part) };
    if low_part == u32::MAX && unsafe { GetLastError() } != NO_ERROR {
        fallback_logical_size
    } else {
        combine_high_low(high_part, low_part)
    }
}

pub fn enumerate_directory(
    path: &str,
    mut on_entry: impl FnMut(&DiscoveredEntry),
    mut on_log: impl FnMut(&str),
) -> Result<(), String> {
    let search_pattern = make_search_pattern(path);
    let mut find_data = MaybeUninit::<WIN32_FIND_DATAW>::zeroed();
    let handle: HANDLE = unsafe { FindFirstFileW(search_pattern.as_ptr(), find_data.as_mut_ptr()) };
    if handle == INVALID_HANDLE_VALUE {
        let error = unsafe { GetLastError() };
        if error == 2 || error == 3 {
            on_log(&format!(
                "step=FindFirstFileMissing path=\"{path}\" error={error}"
            ));
            return Ok(());
        }
        if error == 5 {
            on_log(&format!("step=FindFirstFileAccessDenied path=\"{path}\""));
            return Ok(());
        }

        return Err(format!(
            "FindFirstFileW failed. error={error} path=\"{path}\""
        ));
    }

    let mut done = false;
    while !done {
        let current = unsafe { find_data.assume_init_ref() };
        let child_name = widestring_without_nul(&current.cFileName);
        let is_dot_entry = child_name == [b'.' as u16] || child_name == [b'.' as u16, b'.' as u16];
        if !is_dot_entry {
            let name = child_name.to_vec();
            let full_path = join_child_path(path, child_name);
            let attributes = current.dwFileAttributes;
            let reparse_tag = get_reparse_tag(current);
            let last_write_time = filetime_to_u64(current.ftLastWriteTime);
            let index = try_get_file_index(&full_path, attributes);

            let entry = if (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 {
                DiscoveredEntry {
                    kind: DiscoveredEntryKind::Directory,
                    name,
                    full_path,
                    size_physical: 0,
                    size_logical: 0,
                    index,
                    last_write_time,
                    attributes,
                    reparse_tag,
                    is_reserved: false,
                    is_off_volume: is_off_volume_reparse_point(reparse_tag),
                    is_protected_reparse_point: is_protected_reparse_point(attributes),
                }
            } else {
                let logical_size = get_logical_size(current);
                let physical_size = get_physical_size(&full_path, logical_size);
                DiscoveredEntry {
                    kind: DiscoveredEntryKind::File,
                    name,
                    full_path,
                    size_physical: physical_size,
                    size_logical: logical_size,
                    index,
                    last_write_time,
                    attributes,
                    reparse_tag,
                    is_reserved: false,
                    is_off_volume: false,
                    is_protected_reparse_point: false,
                }
            };

            on_entry(&entry);
        }

        let next_ok = unsafe { FindNextFileW(handle, find_data.as_mut_ptr()) };
        if next_ok == 0 {
            let error = unsafe { GetLastError() };
            if error == 18 {
                done = true;
            } else {
                unsafe { FindClose(handle) };
                return Err(format!(
                    "FindNextFileW failed. error={error} path=\"{path}\""
                ));
            }
        }
    }

    unsafe { FindClose(handle) };
    Ok(())
}

pub fn discover_directory_entries(path: &[u16]) -> Result<Vec<DiscoveredEntry>, String> {
    #[cfg(test)]
    if let Some(override_fn) = TEST_DISCOVERY_OVERRIDE.with(|slot| slot.borrow().clone()) {
        return override_fn(path);
    }

    let path = utf16_path_to_string_lossy(path);
    let mut entries = Vec::new();
    enumerate_directory(&path, |entry| entries.push(entry.clone()), |_| {}).map(|()| entries)
}

#[cfg(test)]
pub fn set_test_discovery_override(
    callback: Option<Rc<dyn Fn(&[u16]) -> Result<Vec<DiscoveredEntry>, String>>>,
) {
    TEST_DISCOVERY_OVERRIDE.with(|slot| {
        *slot.borrow_mut() = callback;
    });
}

pub fn discover_directory(path: impl AsRef<Path>) -> Result<Vec<DiscoveryEntry>, String> {
    let mut entries = Vec::new();
    let path = path.as_ref().to_string_lossy().into_owned();
    enumerate_directory(
        &path,
        |entry| {
            let name = String::from_utf16_lossy(&entry.name);
            let (entry_type, size) = match entry.kind {
                DiscoveredEntryKind::File => (DiscoveryEntryType::File, Some(entry.size_logical)),
                DiscoveredEntryKind::Directory => (DiscoveryEntryType::Directory, None),
            };
            let size_physical = match entry.kind {
                DiscoveredEntryKind::File => Some(entry.size_physical),
                DiscoveredEntryKind::Directory => None,
            };

            entries.push(DiscoveryEntry {
                name,
                entry_type,
                size,
                size_logical: size,
                size_physical,
                index: entry.index,
            });
        },
        |_| {},
    )
    .map(|()| entries)
}
