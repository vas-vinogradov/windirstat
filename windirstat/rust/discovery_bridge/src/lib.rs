use std::cell::RefCell;
use std::ffi::{c_char, c_void, CString, OsString};
use std::mem::MaybeUninit;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::ptr;

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

#[repr(C)]
#[derive(Copy, Clone)]
pub enum RustDiscoveryEntryKind {
    File = 0,
    Directory = 1,
}

#[repr(C)]
pub struct RustDiscoveryEntryView {
    kind: RustDiscoveryEntryKind,
    name: *const u16,
    name_len: usize,
    full_path: *const u16,
    full_path_len: usize,
    size_physical: u64,
    size_logical: u64,
    index: u64,
    last_write_time: u64,
    attributes: u32,
    reparse_tag: u32,
    is_reserved: u8,
    is_off_volume: u8,
    is_protected_reparse_point: u8,
}

#[repr(C)]
pub struct RustDiscoveryErrorView {
    message: *const c_char,
    message_len: usize,
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

type RustDiscoveryEntryCallback =
    Option<unsafe extern "C" fn(entry: *const RustDiscoveryEntryView, user_data: *mut c_void)>;
type RustDiscoveryLogCallback =
    Option<unsafe extern "C" fn(message: *const u16, message_len: usize, user_data: *mut c_void)>;

thread_local! {
    static LAST_ERROR_MESSAGE: RefCell<Option<CString>> = const { RefCell::new(None) };
}

const IO_REPARSE_TAG_MOUNT_POINT_VALUE: u32 = 0xA0000003;
const IO_REPARSE_TAG_SYMLINK_VALUE: u32 = 0xA000000C;

fn utf16_slice_to_string(value: *const u16, len: usize) -> Result<String, &'static str> {
    if value.is_null() {
        return Err("Path pointer was null.");
    }

    let slice = unsafe { std::slice::from_raw_parts(value, len) };
    Ok(String::from_utf16_lossy(slice))
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

fn set_error(error: *mut RustDiscoveryErrorView, message: &str) {
    if error.is_null() {
        return;
    }

    let sanitized = message.replace('\0', " ");
    LAST_ERROR_MESSAGE.with(|slot| {
        let fallback = CString::new("Rust discovery error").expect("static CString creation");
        let cstring = CString::new(sanitized).unwrap_or(fallback);
        let len = cstring.to_bytes().len();
        let ptr = cstring.as_ptr();
        *slot.borrow_mut() = Some(cstring);
        unsafe {
            (*error).message = ptr;
            (*error).message_len = len;
        }
    });
}

fn clear_error(error: *mut RustDiscoveryErrorView) {
    LAST_ERROR_MESSAGE.with(|slot| {
        *slot.borrow_mut() = None;
    });

    if error.is_null() {
        return;
    }

    unsafe {
        (*error).message = ptr::null();
        (*error).message_len = 0;
    }
}

fn emit_log(callback: RustDiscoveryLogCallback, user_data: *mut c_void, message: &str) {
    let Some(callback) = callback else {
        return;
    };

    let sanitized = message.replace('\0', " ");
    let wide: Vec<u16> = OsString::from(sanitized)
        .as_os_str()
        .encode_wide()
        .collect();
    unsafe { callback(wide.as_ptr(), wide.len(), user_data) };
}

fn enumerate_directory(
    path: &str,
    callback: RustDiscoveryEntryCallback,
    user_data: *mut c_void,
    log_callback: RustDiscoveryLogCallback,
    log_user_data: *mut c_void,
) -> Result<(), String> {
    let search_pattern = make_search_pattern(path);
    let mut find_data = MaybeUninit::<WIN32_FIND_DATAW>::zeroed();
    let handle: HANDLE = unsafe { FindFirstFileW(search_pattern.as_ptr(), find_data.as_mut_ptr()) };
    if handle == INVALID_HANDLE_VALUE {
        let error = unsafe { GetLastError() };
        if error == 2 || error == 3 {
            emit_log(
                log_callback,
                log_user_data,
                &format!("step=FindFirstFileMissing path=\"{path}\" error={error}"),
            );
            return Ok(());
        }
        if error == 5 {
            emit_log(
                log_callback,
                log_user_data,
                &format!("step=FindFirstFileAccessDenied path=\"{path}\""),
            );
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
            let full_path = join_child_path(path, child_name);
            let attributes = current.dwFileAttributes;
            let reparse_tag = get_reparse_tag(current);
            let last_write_time = filetime_to_u64(current.ftLastWriteTime);
            let index = try_get_file_index(&full_path, attributes);

            let entry = if (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 {
                RustDiscoveryEntryView {
                    kind: RustDiscoveryEntryKind::Directory,
                    name: child_name.as_ptr(),
                    name_len: child_name.len(),
                    full_path: full_path.as_ptr(),
                    full_path_len: full_path.len(),
                    size_physical: 0,
                    size_logical: 0,
                    index,
                    last_write_time,
                    attributes,
                    reparse_tag,
                    is_reserved: 0,
                    is_off_volume: is_off_volume_reparse_point(reparse_tag) as u8,
                    is_protected_reparse_point: is_protected_reparse_point(attributes) as u8,
                }
            } else {
                let logical_size = get_logical_size(current);
                let physical_size = get_physical_size(&full_path, logical_size);
                RustDiscoveryEntryView {
                    kind: RustDiscoveryEntryKind::File,
                    name: child_name.as_ptr(),
                    name_len: child_name.len(),
                    full_path: full_path.as_ptr(),
                    full_path_len: full_path.len(),
                    size_physical: physical_size,
                    size_logical: logical_size,
                    index,
                    last_write_time,
                    attributes,
                    reparse_tag,
                    is_reserved: 0,
                    is_off_volume: 0,
                    is_protected_reparse_point: 0,
                }
            };

            if let Some(callback) = callback {
                unsafe { callback(&entry, user_data) };
            }
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

pub fn discover_directory(path: impl AsRef<Path>) -> Result<Vec<DiscoveryEntry>, String> {
    unsafe extern "C" fn collect_entry(
        entry: *const RustDiscoveryEntryView,
        user_data: *mut c_void,
    ) {
        if entry.is_null() || user_data.is_null() {
            return;
        }

        let entry = unsafe { &*entry };
        let entries = unsafe { &mut *(user_data as *mut Vec<DiscoveryEntry>) };
        let name = if entry.name.is_null() {
            String::new()
        } else {
            let name_slice = unsafe { std::slice::from_raw_parts(entry.name, entry.name_len) };
            String::from_utf16_lossy(name_slice)
        };
        let (entry_type, size) = match entry.kind {
            RustDiscoveryEntryKind::File => (DiscoveryEntryType::File, Some(entry.size_logical)),
            RustDiscoveryEntryKind::Directory => (DiscoveryEntryType::Directory, None),
        };
        let size_physical = match entry.kind {
            RustDiscoveryEntryKind::File => Some(entry.size_physical),
            RustDiscoveryEntryKind::Directory => None,
        };

        entries.push(DiscoveryEntry {
            name,
            entry_type,
            size,
            size_logical: size,
            size_physical,
            index: entry.index,
        });
    }

    let mut entries = Vec::new();
    let path = path.as_ref().to_string_lossy().into_owned();
    enumerate_directory(
        &path,
        Some(collect_entry),
        &mut entries as *mut Vec<DiscoveryEntry> as *mut c_void,
        None,
        ptr::null_mut(),
    )
    .map(|()| entries)
}

#[no_mangle]
pub extern "C" fn windirstat_rust_discover_directory(
    path: *const u16,
    path_len: usize,
    callback: RustDiscoveryEntryCallback,
    user_data: *mut c_void,
    log_callback: RustDiscoveryLogCallback,
    log_user_data: *mut c_void,
    error: *mut RustDiscoveryErrorView,
) -> bool {
    clear_error(error);

    let result = std::panic::catch_unwind(|| {
        let path = utf16_slice_to_string(path, path_len).map_err(|message| message.to_string())?;
        enumerate_directory(&path, callback, user_data, log_callback, log_user_data)
    });

    match result {
        Ok(Ok(())) => true,
        Ok(Err(message)) => {
            set_error(error, &message);
            false
        }
        Err(_) => {
            set_error(error, "Rust discovery panicked.");
            false
        }
    }
}
