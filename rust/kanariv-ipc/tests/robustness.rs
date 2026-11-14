//! Deterministic robustness tests: no input may panic the frame or reply
//! parsers. This is the in-tree equivalent of a fuzz corpus so it runs in CI
//! without nightly or libFuzzer.

use std::io::Cursor;

use kanariv_ipc::{decode_from, encode, parse_reply, Payload, MAX_FRAME};

/// xorshift64*, chosen for deterministic, dependency-free test data.
struct Rng(u64);

impl Rng {
    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn next_u32(&mut self) -> u32 {
        (self.next_u64() >> 32) as u32
    }

    fn bytes(&mut self, len: usize) -> Vec<u8> {
        (0..len).map(|_| self.next_u32() as u8).collect()
    }
}

#[test]
fn arbitrary_json_never_panics_the_reply_parser() {
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    for _ in 0..5000 {
        let len = (rng.next_u32() % 256) as usize;
        let bytes = rng.bytes(len);
        let _ = parse_reply(&bytes);
    }
}

#[test]
fn bit_flips_of_valid_replies_never_panic() {
    let valid = br#"{"id":1,"ok":true,"result":{"type":"Status","state":"Playing","state_value":2,"pos":1.0,"dur":10.0,"vol":0.5,"muted":false,"shuffle":false,"repeat":"Off","repeat_value":0,"track_id":1,"title":"T","artist":"A","path":"/p","q_size":2,"q_idx":0,"version":"v"}}"#;
    let mut rng = Rng(0xD1B5_4A32_D192_ED03);
    for _ in 0..5000 {
        let mut mutated = valid.to_vec();
        let flips = 1 + (rng.next_u32() % 8) as usize;
        for _ in 0..flips {
            let index = (rng.next_u32() as usize) % mutated.len();
            mutated[index] ^= 1 << (rng.next_u32() % 8);
        }
        let _ = parse_reply(&mutated);
    }
}

#[test]
fn arbitrary_typed_results_never_panic() {
    let types = [
        "Status",
        "QueueTracks",
        "Queues",
        "QueueCreated",
        "PlaylistLoaded",
        "VolumeInfo",
        "LibraryStats",
        "LibraryStatsDetailed",
        "Tracks",
        "SearchResults",
        "ScanReport",
        "Playlists",
        "PlaylistData",
        "PlaylistCreated",
        "PlaylistImportReport",
        "LibraryMaintenanceReport",
        "ConfigValue",
        "ConfigValues",
        "SingleTrack",
        "TrackInfo",
        "HistoryEntry",
        "History",
        "Bookmarks",
        "Lyrics",
        "TagBatchEditResult",
        "Devices",
        "Empty",
        "Future",
        "",
    ];
    let fields = [
        "tracks",
        "queues",
        "playlists",
        "entries",
        "devices",
        "values",
        "results",
        "status",
        "track",
        "state",
        "repeat",
        "vol",
        "added",
        "id",
        "name",
        "path",
        "library_id",
        "total_plays",
    ];
    let values = [
        serde_json::Value::Null,
        serde_json::json!(0),
        serde_json::json!(-1),
        serde_json::json!(1.5),
        serde_json::json!("text"),
        serde_json::json!([]),
        serde_json::json!([null]),
        serde_json::json!([{"id": 1}]),
        serde_json::json!({}),
        serde_json::json!({"type": "Status", "state": "Playing"}),
    ];

    let mut rng = Rng(0x0123_4567_89AB_CDEF);
    for _ in 0..2000 {
        let mut object = serde_json::Map::new();
        object.insert(
            "type".to_string(),
            serde_json::Value::String(types[(rng.next_u32() as usize) % types.len()].to_string()),
        );
        for _ in 0..(rng.next_u32() % 5) {
            let key = fields[(rng.next_u32() as usize) % fields.len()];
            let value = values[(rng.next_u32() as usize) % values.len()].clone();
            object.insert(key.to_string(), value);
        }
        let _ = Payload::from_json(&serde_json::Value::Object(object));
    }
}

#[test]
fn arbitrary_frame_headers_never_panic() {
    let mut rng = Rng(0xA5A5_5A5A_1234_5678);
    for _ in 0..5000 {
        // Keep the declared length small so the test never allocates 16 MiB
        // per iteration; oversized headers have their own boundary test.
        let declared = rng.next_u32() & 0x0FFF;
        let mut bytes = declared.to_be_bytes().to_vec();
        let body = (rng.next_u32() % 32) as usize;
        bytes.extend_from_slice(&rng.bytes(body));
        let _ = decode_from(&mut Cursor::new(bytes));
    }
}

#[test]
fn framed_payloads_round_trip_at_random_lengths() {
    let mut rng = Rng(0xDEAD_BEEF_CAFE_F00D);
    for _ in 0..1000 {
        let len = (rng.next_u32() % 1024) as usize;
        let payload = rng.bytes(len);
        let framed = encode(&payload).expect("within limit");
        assert_eq!(decode_from(&mut Cursor::new(framed)).unwrap(), payload);
    }
}

#[test]
fn frame_boundary_is_enforced_before_allocation() {
    let mut header = ((MAX_FRAME as u32) + 1).to_be_bytes().to_vec();
    header.push(0);
    let err = decode_from(&mut Cursor::new(header)).unwrap_err();
    assert_eq!(err.kind(), std::io::ErrorKind::InvalidData);
    assert!(encode(&vec![0u8; MAX_FRAME + 1]).is_err());
    assert!(encode(&vec![0u8; MAX_FRAME]).is_ok());
}
