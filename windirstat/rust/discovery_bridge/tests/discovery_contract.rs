//! Rust-owned discovery contract and migration parity harness.
//!
//! These tests validate the discovery seam only: a backend receives one path and
//! returns normalized immediate-child entries or a normalized failure. The legacy
//! backend adapter is temporary migration scaffolding around the current scan
//! host, and keeps legacy-specific details out of the shared assertions.

use std::env;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::MetadataExt;
use std::os::windows::io::AsRawHandle;
use std::path::{Path, PathBuf};
use std::process::{self, Child, Command, Stdio};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};
use windirstat_rust_discovery::{discover_directory, DiscoveryEntry, DiscoveryEntryType};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, SetLastError, HANDLE, INVALID_HANDLE_VALUE, NO_ERROR,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, GetCompressedFileSizeW, GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
    FILE_ATTRIBUTE_DIRECTORY, FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT,
    FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
};
use windows_sys::Win32::System::IO::DeviceIoControl;

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

static NEXT_FIXTURE_ID: AtomicU64 = AtomicU64::new(0);
const FSCTL_SET_SPARSE: u32 = 0x0009_00C4;

#[derive(Debug, Clone, Copy)]
struct FileContractMetadata {
    logical_size: u64,
    physical_size: u64,
    index: u64,
}

#[derive(Debug)]
struct Fixture {
    root: PathBuf,
}

impl Fixture {
    fn new() -> io::Result<Self> {
        for _ in 0..100 {
            let id = NEXT_FIXTURE_ID.fetch_add(1, Ordering::Relaxed);
            let timestamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos();
            let root = env::temp_dir().join(format!(
                "windirstat_discovery_contract_{}_{}_{}",
                process::id(),
                timestamp,
                id
            ));

            match fs::create_dir(&root) {
                Ok(()) => return Ok(Self { root }),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }

        Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "could not create a unique discovery test fixture directory",
        ))
    }

    fn path(&self) -> &Path {
        &self.root
    }

    fn file(&self, relative_path: impl AsRef<Path>, contents: &[u8]) -> io::Result<()> {
        let path = self.entry_path(relative_path);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(path, contents)
    }

    fn dir(&self, relative_path: impl AsRef<Path>) -> io::Result<()> {
        fs::create_dir_all(self.entry_path(relative_path))
    }

    fn entry_path(&self, relative_path: impl AsRef<Path>) -> PathBuf {
        let relative_path = relative_path.as_ref();
        assert!(
            relative_path.is_relative(),
            "fixture entries must be relative paths"
        );
        self.root.join(relative_path)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

type NormalizedDiscoveryResult = Result<Vec<NormalizedDiscoveryEntry>, NormalizedDiscoveryFailure>;
type DiscoveryResult = TestResult<NormalizedDiscoveryResult>;
type DiscoverFn = fn(&Path) -> DiscoveryResult;

// Contract: a backend exposes only the discovery seam: path in, normalized
// immediate-child results or an error out.
#[derive(Clone, Copy)]
struct DiscoveryBackend {
    name: &'static str,
    discover: DiscoverFn,
}

impl DiscoveryBackend {
    fn discover(&self, path: &Path) -> DiscoveryResult {
        (self.discover)(path)
    }
}

// Contract: the shared failure shape records only portable seam facts. Backend
// adapters may derive a category, but exact error text is intentionally ignored.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct NormalizedDiscoveryFailure {
    category: Option<DiscoveryFailureCategory>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DiscoveryFailureCategory {
    InvalidInput,
}

fn normalize_discovery_failure(path: &Path) -> NormalizedDiscoveryFailure {
    normalize_input_failure(path).unwrap_or(NormalizedDiscoveryFailure { category: None })
}

fn normalize_input_failure(path: &Path) -> Option<NormalizedDiscoveryFailure> {
    match path.try_exists() {
        Err(error) if error.kind() == io::ErrorKind::InvalidFilename => {
            Some(NormalizedDiscoveryFailure {
                category: Some(DiscoveryFailureCategory::InvalidInput),
            })
        }
        _ => None,
    }
}

fn discover_with_rust(path: &Path) -> DiscoveryResult {
    match discover_directory(path) {
        Ok(entries) => Ok(Ok(entries
            .into_iter()
            .map(NormalizedDiscoveryEntry::from)
            .collect())),
        Err(_message) => Ok(Err(normalize_discovery_failure(path))),
    }
}

// Temporary: migration scaffold that adapts the existing scan host's legacy
// discovery mode into the Rust-owned seam contract. Keep it disposable and
// remove it once legacy parity is no longer needed.
fn discover_with_legacy(path: &Path) -> DiscoveryResult {
    const REQUEST_ID: u64 = 1;

    let host = LegacyDiscoveryHost::start()?;
    let mut pipe = connect_to_named_pipe(&host.pipe_name)?;
    let requested_path = path.to_string_lossy().into_owned();
    // Temporary: legacy discovery can surface invalid path syntax as an empty
    // progress snapshot, so the adapter normalizes that before returning.
    let preflight_input_failure = normalize_input_failure(path);

    send_rpc_message(
        &mut pipe,
        json!({
            "kind": "StartScanRequest",
            "requestId": REQUEST_ID,
            "rootPath": requested_path,
            "followMountPoints": false,
            "followSymbolicLinks": false,
            "followJunctions": false,
        }),
    )?;

    for _ in 0..16 {
        let event = read_rpc_message(&mut pipe)?;
        match json_string(&event, "kind")? {
            "DirectoryProgressEvent" => {
                if json_string(&event, "directoryPath")? == requested_path {
                    send_rpc_message(
                        &mut pipe,
                        json!({
                            "kind": "CancelScanRequest",
                            "requestId": REQUEST_ID,
                            "reason": 1,
                        }),
                    )?;
                    if let Some(failure) = preflight_input_failure {
                        return Ok(Err(failure));
                    }

                    return Ok(Ok(normalize_legacy_progress_event(&event)?));
                }
            }
            "ScanFailedEvent" => {
                return Ok(Err(normalize_discovery_failure(path)));
            }
            _ => {}
        }
    }

    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        format!(
            "legacy discovery did not return a progress event for {}",
            path.display()
        ),
    )
    .into())
}

fn migration_parity_backends() -> &'static [DiscoveryBackend] {
    &[
        DiscoveryBackend {
            name: "rust",
            discover: discover_with_rust,
        },
        DiscoveryBackend {
            name: "legacy",
            discover: discover_with_legacy,
        },
    ]
}

struct LegacyDiscoveryHost {
    child: Child,
    pipe_name: String,
    log_path: PathBuf,
}

impl LegacyDiscoveryHost {
    fn start() -> TestResult<Self> {
        let id = NEXT_FIXTURE_ID.fetch_add(1, Ordering::Relaxed);
        let pipe_name = format!(
            r"\\.\pipe\WinDirStat.LegacyDiscoveryContract.{}.{}",
            process::id(),
            id
        );
        let log_path = env::temp_dir().join(format!(
            "windirstat_legacy_discovery_contract_{}_{}.log",
            process::id(),
            id
        ));
        let host_path = legacy_scan_host_path()?;
        let child = Command::new(&host_path)
            .arg("--pipe")
            .arg(&pipe_name)
            .arg("--log-file")
            .arg(&log_path)
            .env("WINDIRSTAT_REMOTE_DISCOVERY_ENGINE", "legacy")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|error| {
                io::Error::new(
                    error.kind(),
                    format!(
                        "failed to start legacy scan host {}: {error}",
                        host_path.display()
                    ),
                )
            })?;

        Ok(Self {
            child,
            pipe_name,
            log_path,
        })
    }
}

impl Drop for LegacyDiscoveryHost {
    fn drop(&mut self) {
        if matches!(self.child.try_wait(), Ok(None)) {
            let _ = self.child.kill();
        }
        let _ = self.child.wait();
        let _ = fs::remove_file(&self.log_path);
    }
}

fn legacy_scan_host_path() -> io::Result<PathBuf> {
    let host_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .join("Build")
        .join("windirstat-scan-host_x64.exe");
    if host_path.is_file() {
        return Ok(host_path);
    }

    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!(
            "legacy scan host not found at {}; build windirstat-scan-host_x64.exe before running legacy parity tests",
            host_path.display()
        ),
    ))
}

fn connect_to_named_pipe(pipe_name: &str) -> io::Result<File> {
    let started = Instant::now();
    let timeout = Duration::from_secs(5);
    let mut last_error = None;

    while started.elapsed() < timeout {
        match fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(pipe_name)
        {
            Ok(pipe) => return Ok(pipe),
            Err(error) => {
                last_error = Some(error);
                thread::sleep(Duration::from_millis(50));
            }
        }
    }

    Err(io::Error::new(
        last_error
            .as_ref()
            .map(io::Error::kind)
            .unwrap_or(io::ErrorKind::TimedOut),
        format!(
            "timed out connecting to legacy discovery pipe {pipe_name}: {}",
            last_error
                .map(|error| error.to_string())
                .unwrap_or_else(|| "no connection attempt completed".to_owned())
        ),
    ))
}

fn send_rpc_message(pipe: &mut File, message: Value) -> io::Result<()> {
    let json = serde_json::to_vec(&message).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("failed to serialize RPC message: {error}"),
        )
    })?;
    let length = u32::try_from(json.len()).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "RPC message exceeded 32-bit frame length",
        )
    })?;

    pipe.write_all(&length.to_le_bytes())?;
    pipe.write_all(&json)?;
    pipe.flush()
}

fn read_rpc_message(pipe: &mut File) -> TestResult<Value> {
    let mut length_bytes = [0u8; 4];
    pipe.read_exact(&mut length_bytes)?;
    let length = u32::from_le_bytes(length_bytes) as usize;
    let mut json = vec![0u8; length];
    pipe.read_exact(&mut json)?;
    Ok(serde_json::from_slice(&json)?)
}

fn json_string<'a>(value: &'a Value, field: &str) -> io::Result<&'a str> {
    value.get(field).and_then(Value::as_str).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("RPC event missing string field {field}"),
        )
    })
}

fn json_array<'a>(value: &'a Value, field: &str) -> io::Result<&'a [Value]> {
    value
        .get(field)
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("RPC event missing array field {field}"),
            )
        })
}

fn json_u64(value: &Value, field: &str) -> io::Result<u64> {
    value.get(field).and_then(Value::as_u64).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("RPC event missing integer field {field}"),
        )
    })
}

fn path_to_wide_nul(path: &Path) -> Vec<u16> {
    path.as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn combine_high_low(high: u32, low: u32) -> u64 {
    ((high as u64) << 32) | (low as u64)
}

fn close_handle(handle: HANDLE) {
    if handle != INVALID_HANDLE_VALUE && !handle.is_null() {
        unsafe {
            CloseHandle(handle);
        }
    }
}

fn open_metadata_handle(path: &Path, attributes: u32) -> io::Result<HANDLE> {
    let wide_path = path_to_wide_nul(path);
    let flags = FILE_FLAG_OPEN_REPARSE_POINT
        | if (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 {
            FILE_FLAG_BACKUP_SEMANTICS
        } else {
            0
        };
    let handle = unsafe {
        CreateFileW(
            wide_path.as_ptr(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            ptr::null(),
            OPEN_EXISTING,
            flags,
            ptr::null_mut(),
        )
    };

    if handle == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }

    Ok(handle)
}

fn query_file_contract_metadata(path: &Path) -> io::Result<FileContractMetadata> {
    let metadata = fs::metadata(path)?;
    let attributes = metadata.file_attributes();
    let handle = open_metadata_handle(path, attributes)?;

    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    let success = unsafe { GetFileInformationByHandle(handle, &mut info) != 0 };
    close_handle(handle);
    if !success {
        return Err(io::Error::last_os_error());
    }

    let wide_path = path_to_wide_nul(path);
    let mut physical_size_high = 0;
    unsafe {
        SetLastError(NO_ERROR);
    }
    let physical_size_low =
        unsafe { GetCompressedFileSizeW(wide_path.as_ptr(), &mut physical_size_high) };
    let physical_size = if physical_size_low == u32::MAX && unsafe { GetLastError() } != NO_ERROR {
        metadata.len()
    } else {
        combine_high_low(physical_size_high, physical_size_low)
    };

    Ok(FileContractMetadata {
        logical_size: metadata.len(),
        physical_size,
        index: combine_high_low(info.nFileIndexHigh, info.nFileIndexLow),
    })
}

fn expected_file(
    fixture: &Fixture,
    relative_path: impl AsRef<Path>,
) -> TestResult<NormalizedDiscoveryEntry> {
    let relative_path = relative_path.as_ref();
    let path = fixture.entry_path(relative_path);
    let metadata = query_file_contract_metadata(&path)?;
    assert_ne!(
        metadata.index,
        0,
        "fixture filesystem did not provide a stable file identity for {}",
        path.display()
    );
    let name = relative_path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "expected file name"))?;

    Ok(file(
        name,
        metadata.logical_size,
        metadata.physical_size,
        metadata.index,
    ))
}

fn make_sparse_file(path: &Path, logical_size: u64) -> io::Result<bool> {
    let file = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(path)?;
    let handle = file.as_raw_handle() as HANDLE;
    let mut bytes_returned = 0;
    let made_sparse = unsafe {
        DeviceIoControl(
            handle,
            FSCTL_SET_SPARSE,
            ptr::null(),
            0,
            ptr::null_mut(),
            0,
            &mut bytes_returned,
            ptr::null_mut(),
        ) != 0
    };
    if !made_sparse {
        return Ok(false);
    }

    file.set_len(logical_size)?;
    Ok(true)
}

fn normalize_legacy_progress_event(event: &Value) -> TestResult<Vec<NormalizedDiscoveryEntry>> {
    let mut entries = Vec::new();
    for file_value in json_array(event, "files")? {
        entries.push(file(
            json_string(file_value, "name")?,
            json_u64(file_value, "sizeLogical")?,
            json_u64(file_value, "sizePhysical")?,
            json_u64(file_value, "index")?,
        ));
    }

    for directory_value in json_array(event, "directories")? {
        entries.push(dir(json_string(directory_value, "name")?));
    }

    Ok(entries)
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct NormalizedDiscoveryEntry {
    name: String,
    entry_type: NormalizedEntryType,
    logical_size: Option<u64>,
    physical_size: Option<u64>,
    index: Option<u64>,
}

impl From<DiscoveryEntry> for NormalizedDiscoveryEntry {
    fn from(entry: DiscoveryEntry) -> Self {
        Self {
            name: entry.name,
            entry_type: match entry.entry_type {
                DiscoveryEntryType::File => NormalizedEntryType::File,
                DiscoveryEntryType::Directory => NormalizedEntryType::Directory,
            },
            logical_size: entry.size_logical,
            physical_size: entry.size_physical,
            index: match entry.entry_type {
                DiscoveryEntryType::File => Some(entry.index),
                DiscoveryEntryType::Directory => None,
            },
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NormalizedEntryType {
    File,
    Directory,
}

fn normalize(mut entries: Vec<NormalizedDiscoveryEntry>) -> Vec<NormalizedDiscoveryEntry> {
    entries.sort_by(|left, right| left.name.cmp(&right.name));
    entries
}

fn normalize_for_legacy_parity(
    mut entries: Vec<NormalizedDiscoveryEntry>,
) -> Vec<NormalizedDiscoveryEntry> {
    for entry in &mut entries {
        entry.physical_size = None;
        entry.index = None;
    }

    normalize(entries)
}

fn file(name: &str, logical_size: u64, physical_size: u64, index: u64) -> NormalizedDiscoveryEntry {
    NormalizedDiscoveryEntry {
        name: name.to_owned(),
        entry_type: NormalizedEntryType::File,
        logical_size: Some(logical_size),
        physical_size: Some(physical_size),
        index: Some(index),
    }
}

fn dir(name: &str) -> NormalizedDiscoveryEntry {
    NormalizedDiscoveryEntry {
        name: name.to_owned(),
        entry_type: NormalizedEntryType::Directory,
        logical_size: None,
        physical_size: None,
        index: None,
    }
}

fn assert_migration_parity_matches(
    fixture: &Fixture,
    expected: Vec<NormalizedDiscoveryEntry>,
) -> TestResult {
    assert_discovery_matches_with(
        migration_parity_backends(),
        fixture.path(),
        expected,
        normalize_for_legacy_parity,
    )
}

fn assert_rust_discovery_matches(
    fixture: &Fixture,
    expected: Vec<NormalizedDiscoveryEntry>,
) -> TestResult {
    assert_discovery_matches_with(
        &[DiscoveryBackend {
            name: "rust",
            discover: discover_with_rust,
        }],
        fixture.path(),
        expected,
        normalize,
    )
}

fn assert_discovery_matches_with(
    backends: &[DiscoveryBackend],
    path: &Path,
    expected: Vec<NormalizedDiscoveryEntry>,
    normalize_entries: fn(Vec<NormalizedDiscoveryEntry>) -> Vec<NormalizedDiscoveryEntry>,
) -> TestResult {
    let expected = normalize_entries(expected);
    assert!(
        !backends.is_empty(),
        "at least one discovery backend must run"
    );

    for backend in backends {
        let actual = backend.discover(path)?.map(normalize_entries);
        assert_eq!(actual, Ok(expected.clone()), "backend {}", backend.name);
    }

    Ok(())
}

fn assert_discovery_fails(path: &Path) -> TestResult {
    assert_discovery_fails_with(
        migration_parity_backends(),
        path,
        NormalizedDiscoveryFailure {
            category: Some(DiscoveryFailureCategory::InvalidInput),
        },
    )
}

fn assert_discovery_fails_with(
    backends: &[DiscoveryBackend],
    path: &Path,
    expected: NormalizedDiscoveryFailure,
) -> TestResult {
    assert!(
        !backends.is_empty(),
        "at least one discovery backend must run"
    );

    for backend in backends {
        let actual = backend.discover(path)?;
        assert_eq!(actual, Err(expected), "backend {}", backend.name);
    }

    Ok(())
}

// These tests exercise only the discovery seam: real filesystem fixtures,
// immediate children, normalized order, and contract-relevant fields.

// Assumption: an empty real directory has no contract-relevant immediate
// children.
#[test]
fn empty_directory() -> TestResult {
    let fixture = Fixture::new()?;

    assert_migration_parity_matches(&fixture, vec![])
}

// Assumption: file byte length is contract-relevant, while directories carry
// no size.
#[test]
fn directory_with_one_file_and_one_subdirectory() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.file("sample.txt", b"hello")?;
    fixture.dir("child")?;

    assert_migration_parity_matches(
        &fixture,
        vec![expected_file(&fixture, "sample.txt")?, dir("child")],
    )
}

// Assumption: logical byte count and Windows-reported physical allocation are
// both contract-relevant file-size outputs.
#[test]
fn file_size_reports_known_byte_count() -> TestResult {
    const KNOWN_SIZE: u64 = 4097;

    let fixture = Fixture::new()?;
    let contents = vec![0x5A; KNOWN_SIZE as usize];
    fixture.file("known-size.bin", &contents)?;
    let expected = expected_file(&fixture, "known-size.bin")?;
    assert_eq!(expected.logical_size, Some(KNOWN_SIZE));

    assert_migration_parity_matches(&fixture, vec![expected])
}

// Assumption: when Windows exposes a distinct allocated size, Rust discovery
// reports that physical size instead of reusing the logical byte count.
#[test]
fn sparse_file_reports_distinct_physical_size_when_supported() -> TestResult {
    const LOGICAL_SIZE: u64 = 1024 * 1024;

    let fixture = Fixture::new()?;
    let path = fixture.entry_path("sparse.bin");
    if !make_sparse_file(&path, LOGICAL_SIZE)? {
        eprintln!(
            "skipping sparse physical-size assertion; fixture filesystem does not support sparse files"
        );
        return Ok(());
    }

    let expected = expected_file(&fixture, "sparse.bin")?;
    assert_eq!(expected.logical_size, Some(LOGICAL_SIZE));
    assert_ne!(
        expected.physical_size, expected.logical_size,
        "sparse fixture did not expose a distinct physical size"
    );

    assert_rust_discovery_matches(&fixture, vec![expected])
}

// Assumption: hardlinked files share the same nonzero Windows file identity,
// which downstream hardlink accounting depends on.
#[test]
fn hardlinked_files_report_shared_file_index() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.file("original.bin", b"same identity")?;
    fs::hard_link(
        fixture.entry_path("original.bin"),
        fixture.entry_path("linked.bin"),
    )?;

    let original = expected_file(&fixture, "original.bin")?;
    let linked = expected_file(&fixture, "linked.bin")?;
    assert_eq!(original.index, linked.index);

    assert_rust_discovery_matches(&fixture, vec![original, linked])
}

// Assumption: filesystem enumeration order is not contract-relevant, but every
// direct child is.
#[test]
fn directory_with_multiple_entries() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.file("zeta.bin", b"1234567")?;
    fixture.file("alpha.txt", b"")?;
    fixture.file("notes.md", b"contract")?;
    fixture.dir("media")?;
    fixture.dir("src")?;

    assert_migration_parity_matches(
        &fixture,
        vec![
            expected_file(&fixture, "zeta.bin")?,
            expected_file(&fixture, "alpha.txt")?,
            expected_file(&fixture, "notes.md")?,
            dir("media"),
            dir("src"),
        ],
    )
}

// Assumption: discovery owns only one directory listing and does not recurse
// into child directories.
#[test]
fn directory_with_nested_entries_reports_only_immediate_children() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.file("root.txt", b"root")?;
    fixture.file("child/nested.txt", b"nested")?;
    fixture.dir("child/grandchild")?;

    assert_migration_parity_matches(
        &fixture,
        vec![expected_file(&fixture, "root.txt")?, dir("child")],
    )
}

// Assumption: Unicode child names round-trip through platform discovery without
// changing file or directory identity.
#[test]
fn directory_with_unicode_names() -> TestResult {
    let fixture = Fixture::new()?;
    let file_name = "caf\u{00e9}.txt";
    let dir_name = "\u{65e5}\u{672c}\u{8a9e}";
    fixture.file(file_name, b"unicode")?;
    fixture.dir(dir_name)?;

    assert_migration_parity_matches(
        &fixture,
        vec![expected_file(&fixture, file_name)?, dir(dir_name)],
    )
}

// Contract: invalid input paths fail discovery with a normalized category, but
// backend-specific messages and error codes are not part of this seam test.
#[test]
fn invalid_input_path_returns_error() -> TestResult {
    let fixture = Fixture::new()?;
    let invalid_path = fixture.path().join("invalid\"name");

    assert_discovery_fails(&invalid_path)
}
