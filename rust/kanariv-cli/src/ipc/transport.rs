//! Connection setup for Unix domain sockets and Windows named pipes.

use std::io::{self, Read, Write};

#[cfg(unix)]
mod imp {
    pub type Raw = std::os::unix::net::UnixStream;

    pub fn connect(path: &str) -> io::Result<Raw> {
        std::os::unix::net::UnixStream::connect(path)
    }
}

#[cfg(windows)]
mod imp {
    use std::fs::OpenOptions;
    use std::io;
    use std::time::Duration;

    pub type Raw = std::fs::File;

    // ERROR_PIPE_BUSY: a live listener whose instances are momentarily
    // taken. Wait briefly and retry once instead of reporting "not running".
    const ERROR_PIPE_BUSY: i32 = 231;

    pub fn connect(path: &str) -> io::Result<Raw> {
        match open(path) {
            Ok(file) => Ok(file),
            Err(err) if err.raw_os_error() == Some(ERROR_PIPE_BUSY) => {
                std::thread::sleep(Duration::from_millis(500));
                open(path)
            }
            Err(err) => Err(err),
        }
    }

    fn open(path: &str) -> io::Result<Raw> {
        OpenOptions::new().read(true).write(true).open(path)
    }
}

pub struct Connection {
    stream: imp::Raw,
}

impl Connection {
    pub fn connect(path: &str) -> io::Result<Self> {
        Ok(Self {
            stream: imp::connect(path)?,
        })
    }

    pub fn send_bytes(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.stream.write_all(bytes)?;
        self.stream.flush()
    }

    pub fn recv_bytes(&mut self) -> io::Result<Vec<u8>> {
        super::frame::decode_from(&mut self.stream)
    }
}

impl Read for Connection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.stream.read(buf)
    }
}

impl Write for Connection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.stream.write(buf)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.stream.flush()
    }
}
