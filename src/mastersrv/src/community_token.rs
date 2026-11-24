use arrayvec::ArrayString;
use constant_time_eq::constant_time_eq;
use sha2::Digest as _;
use sha2::Sha256;
use std::str::FromStr;

fn strip_crc32(s: &str) -> Option<&str> {
    if s.len() < 6 || !s.is_char_boundary(s.len() - 6) {
        return None;
    }
    let (result, encoded_crc32) = s.split_at(s.len() - 6);
    let mut crc32 = [0; 4];
    if base64::decode_config_slice(encoded_crc32, base64::URL_SAFE_NO_PAD, &mut crc32).ok()?
        != crc32.len()
    {
        return None;
    }
    if crc32fast::hash(result.as_bytes()).to_be_bytes() != crc32 {
        return None;
    }
    Some(result)
}

fn token(s: &str) -> Option<(&str, &str)> {
    let s = strip_crc32(s)?;
    let mut underscore = None;
    for (i, b) in s.bytes().enumerate() {
        match b {
            b'0'..=b'9' => {}
            b'A'..=b'Z' => {}
            b'a'..=b'z' => {}
            b'-' => return None, // tokens are generated to not contain -
            b'_' if underscore.is_none() => underscore = Some(i),
            b'_' => return None, // only one underscore allowed
            _ => return None,    // invalid base64 urlsafe
        }
    }
    let underscore = underscore?;
    Some((&s[..underscore], &s[underscore + 1..]))
}

const PLAIN_PREFIX_LEN: usize = 6;
const TOKEN_LEN: usize = 30;
const VERIFICATION_LEN: usize = 28;

pub fn get_plain_prefix_from_token(s: &str) -> Option<&str> {
    let (prefix, token) = token(s)?;
    if prefix != "ddtc" || token.len() != TOKEN_LEN || !token.is_char_boundary(PLAIN_PREFIX_LEN) {
        return None;
    }
    Some(&token[..PLAIN_PREFIX_LEN])
}

pub type PlainPrefix = ArrayString<[u8; PLAIN_PREFIX_LEN]>;
pub struct Hash([u8; 16]);

fn half_sha256(input: &[u8]) -> Hash {
    let sha256: [u8; 32] = Sha256::digest(input).into();
    let mut half = [0; 16];
    half.copy_from_slice(&sha256[..16]);
    Hash(half)
}

impl Hash {
    pub fn verify(&self, token: &str) -> Result<(), ()> {
        if self.constant_time_eq(&half_sha256(token.as_bytes())) {
            Ok(())
        } else {
            Err(())
        }
    }
    fn constant_time_eq(&self, other: &Hash) -> bool {
        constant_time_eq(&self.0, &other.0)
    }
}

pub struct Verification {
    pub plain_prefix: PlainPrefix,
    pub hash: Hash,
}

impl Verification {
    fn from_str_impl(s: &str) -> Option<Verification> {
        let (prefix, token) = token(s)?;
        if prefix != "ddvc"
            || token.len() != VERIFICATION_LEN
            || !token.is_char_boundary(PLAIN_PREFIX_LEN)
        {
            return None;
        }
        let (plain_prefix, hash_base64) = token.split_at(PLAIN_PREFIX_LEN);
        let plain_prefix = PlainPrefix::from(plain_prefix).ok()?;
        let mut hash = Hash([0; 16]);
        if base64::decode_config_slice(hash_base64, base64::URL_SAFE_NO_PAD, &mut hash.0).ok()?
            != hash.0.len()
        {
            return None;
        }
        Some(Verification { plain_prefix, hash })
    }
}

impl FromStr for Verification {
    type Err = &'static str;
    fn from_str(s: &str) -> Result<Verification, &'static str> {
        Verification::from_str_impl(s).ok_or("invalid verification token")
    }
}

#[cfg(test)]
mod tests {
    use super::get_plain_prefix_from_token;
    use super::Verification;

    fn append_crc32(payload: &str) -> String {
        let crc32 = crc32fast::hash(payload.as_bytes()).to_be_bytes();
        format!(
            "{}{}",
            payload,
            base64::encode_config(crc32, base64::URL_SAFE_NO_PAD)
        )
    }

    #[test]
    fn verification() {
        let token = "ddtc_6DnZq5Ix0J2kvDHbkPNtb6bsZxOVQg4ly2jw";
        let verification = "ddvc_6DnZq51fypqX9ldrEFCF9aJdpi6wjgh6YA";

        let verification: Verification = verification.parse().unwrap();
        assert_eq!(
            get_plain_prefix_from_token(token).unwrap(),
            &*verification.plain_prefix
        );
        assert!(verification.hash.verify(token).is_ok());
    }

    #[test]
    fn parses_regular_crc_valid_community_token() {
        let payload = format!("ddtc_{}", "A".repeat(super::TOKEN_LEN));
        let token = append_crc32(&payload);
        assert_eq!(get_plain_prefix_from_token(&token), Some("AAAAAA"));
    }

    #[test]
    fn rejects_crc_valid_token_without_payload_separator() {
        let payload = (0..100_000)
            .map(|index| format!("ddtc{}", index))
            .find(|candidate| {
                let crc32 = crc32fast::hash(candidate.as_bytes()).to_be_bytes();
                let encoded = base64::encode_config(crc32, base64::URL_SAFE_NO_PAD);
                encoded.starts_with('_')
                    && encoded[1..]
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric())
            })
            .expect("test payload with an underscore-leading CRC");
        let token = append_crc32(&payload);
        assert_eq!(token.bytes().filter(|&byte| byte == b'_').count(), 1);
        assert_eq!(get_plain_prefix_from_token(&token), None);
    }

    #[test]
    fn rejects_invalid_characters_and_bad_crc() {
        let invalid_payload_token = append_crc32("ddtc_A+AAAA");
        assert_eq!(get_plain_prefix_from_token(&invalid_payload_token), None);

        let payload = format!("ddtc_{}", "B".repeat(super::TOKEN_LEN));
        let mut bad_crc_token = append_crc32(&payload);
        let replacement = if bad_crc_token.as_bytes()[payload.len()] == b'A' {
            "B"
        } else {
            "A"
        };
        bad_crc_token.replace_range(payload.len()..payload.len() + 1, replacement);
        assert_eq!(get_plain_prefix_from_token(&bad_crc_token), None);
    }
}
