#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_source="/tmp/uce-oauth-es256-native-$$.cpp"
test_binary="/tmp/uce-oauth-es256-native-$$"
cleanup() { rm -f "$test_source" "$test_binary"; }
trap cleanup EXIT

cat >"$test_source" <<'EOF'
#include "src/lib/types.cpp"
#include "src/lib/dvalue.cpp"
#include "src/lib/functionlib.cpp"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <memory>

String base64_encode(String raw)
{
	if(raw.empty()) return("");
	String out(4 * ((raw.size() + 2) / 3), 0);
	int size = EVP_EncodeBlock((unsigned char*)out.data(), (const unsigned char*)raw.data(), (int)raw.size());
	out.resize(size > 0 ? (size_t)size : 0);
	return(out);
}

String base64_decode(String raw, bool& ok)
{
	ok = false;
	if(raw.empty() || raw.size() % 4) return("");
	String out(3 * raw.size() / 4, 0);
	int size = EVP_DecodeBlock((unsigned char*)out.data(), (const unsigned char*)raw.data(), (int)raw.size());
	if(size < 0) return("");
	while(!raw.empty() && raw.back() == '=') { size--; raw.pop_back(); }
	out.resize((size_t)size); ok = true; return(out);
}

#include "src/lib/hash.cpp"

static String b64url_encode(String text)
{
	String out=replace(replace(base64_encode(text),"+","-"),"/","_"); while(!out.empty()&&out.back()=='=') out.pop_back(); return out;
}

static String b64url_decode(String text)
{
	text = replace(replace(text, "-", "+"), "_", "/");
	while(text.size() % 4) text += "=";
	bool ok = false;
	String result = base64_decode(text, ok);
	return(ok ? result : String(""));
}

static bool verify(DValue public_jwk, String jwt)
{
	StringList parts = split(jwt, ".");
	if(parts.size() != 3) return(false);
	String x = b64url_decode(public_jwk["x"].to_string());
	String y = b64url_decode(public_jwk["y"].to_string());
	String raw = b64url_decode(parts[2]);
	if(x.size() != 32 || y.size() != 32 || raw.size() != 64) return(false);
	unsigned char point[65] = {4}; memcpy(point + 1, x.data(), 32); memcpy(point + 33, y.data(), 32);
	OSSL_PARAM params[] = { OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char*)"prime256v1", 0), OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)), OSSL_PARAM_construct_end() };
	EVP_PKEY_CTX* build = EVP_PKEY_CTX_new_from_name(0, "EC", 0); EVP_PKEY* key = 0;
	if(!build || EVP_PKEY_fromdata_init(build) <= 0 || EVP_PKEY_fromdata(build, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) { EVP_PKEY_CTX_free(build); return(false); }
	EVP_PKEY_CTX_free(build);
	BIGNUM* r = BN_bin2bn((const unsigned char*)raw.data(), 32, 0); BIGNUM* s = BN_bin2bn((const unsigned char*)raw.data() + 32, 32, 0);
	ECDSA_SIG* sig = ECDSA_SIG_new(); int der_size = r && s && sig && ECDSA_SIG_set0(sig, r, s) ? i2d_ECDSA_SIG(sig, 0) : 0; r = s = 0;
	String der(der_size > 0 ? (size_t)der_size : 0, 0); unsigned char* out = (unsigned char*)der.data();
	bool ok = der_size > 0 && i2d_ECDSA_SIG(sig, &out) == der_size;
	EVP_MD_CTX* verify_ctx = EVP_MD_CTX_new();
	String signing_input = parts[0] + "." + parts[1];
	ok = ok && verify_ctx && EVP_DigestVerifyInit(verify_ctx, 0, EVP_sha256(), 0, key) > 0 && EVP_DigestVerify(verify_ctx, (const unsigned char*)der.data(), der.size(), (const unsigned char*)signing_input.data(), signing_input.size()) == 1;
	EVP_MD_CTX_free(verify_ctx); ECDSA_SIG_free(sig); EVP_PKEY_free(key); return(ok);
}

int main()
{
	DValue key_request; key_request["operation"] = "key_generate"; key_request["algorithm"] = "ES256";
	DValue key = crypto_operation_native(key_request);
	DValue header; header["alg"] = "none"; header["kid"] = key["kid"]; DValue claims; claims["iss"] = "https://client.example";
	auto sign = [&](DValue private_jwk) { DValue request; request["operation"] = "jwt_sign"; request["algorithm"] = "ES256"; request["private_jwk"] = private_jwk; request["protected_header"] = header; request["claims"] = claims; return(crypto_operation_native(request)); };
	DValue signed_result = sign(key["private_jwk"]); String jwt = signed_result["jwt"].to_string();
	DValue wrong_curve = key["private_jwk"]; wrong_curve["crv"] = "P-384";
	DValue malformed = key["private_jwk"]; malformed["x"] = "bad=";
	DValue mismatch = key["private_jwk"]; String d = mismatch["d"].to_string(); d[0] = d[0] == 'A' ? 'B' : 'A'; mismatch["d"] = d;
	const String cose="pQECAyYgASFYIGsX0fLhLEJH-Lzm5WOkQPJ3A32BLeszoPShOUXYmMKWIlggT-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU", message="d2ViYXV0aG4gZml4ZWQgbWVzc2FnZQ", signature="MEUCIQCkatZK1VVsjk17uvyzyhjdAkMNWXPjxSOMqWcjmM_8XAIgDaSk3Qufyd0_6r9Dm9A8RQbFco-FdTBulq7bvRGoBC4";
	auto op = [&](String operation) { DValue r; r["operation"]=operation; r["algorithm"]="ES256"; return r; };
	auto parse = [&](String key) { DValue r=op("cose_es256_parse"); r["cose_key_base64url"]=key; return crypto_operation_native(r); };
	auto verify_cose = [&](String key,String msg,String sig) { DValue r=op("es256_verify"); r["cose_key_base64url"]=key; r["message_base64url"]=msg; r["signature_der_base64url"]=sig; return crypto_operation_native(r); };
	auto decode_cbor = [&](String bytes) { DValue r=op("cbor_decode"); r["cbor_base64url"]=b64url_encode(bytes); return crypto_operation_native(r); };
	DValue unsupported; unsupported["operation"] = "encrypt"; unsupported["algorithm"] = "ES256";
	DValue unknown_algorithm; unknown_algorithm["operation"] = "key_generate"; unknown_algorithm["algorithm"] = "none";
	DValue untyped_algorithm = key_request; untyped_algorithm["algorithm"] = (f64)256;
	DValue list_header; list_header.set_array(); DValue list_item; list_item = "not-an-object"; list_header.push(list_item); DValue list_request; list_request["operation"] = "jwt_sign"; list_request["algorithm"] = "ES256"; list_request["private_jwk"] = key["private_jwk"]; list_request["protected_header"] = list_header; list_request["claims"] = claims;
	DValue control_request = list_request; control_request["protected_header"] = header; control_request["claims"] = claims; control_request["claims"]["bad"] = String("control\nbyte");
	DValue oversized = key_request; oversized["ignored"] = String(33000, 'x');
	DValue nonfinite = key_request; nonfinite["ignored"] = std::numeric_limits<f64>::quiet_NaN();
	String tampered = jwt; if(tampered.size() > 2) tampered[tampered.size() - 2] = tampered[tampered.size() - 2] == 'A' ? 'B' : 'A';
	bool kid_ok = key["ok"].to_bool() && key["kid"].to_string() == key["thumbprint"].to_string();
	bool signed_ok = signed_result["ok"].to_bool() && jwt != "" && verify(key["public_jwk"], jwt);
	bool tamper_ok = !verify(key["public_jwk"], tampered);
	auto cbor_error = [&](String bytes) { return(decode_cbor(bytes)["error"].to_string() == "invalid_cbor"); };
	auto cose_error = [&](String encoded) { return(parse(encoded)["error"].to_string() == "invalid_cose_key"); };
	String cose_raw = b64url_decode(cose);
	String wrong_kty = cose_raw; wrong_kty[2] = 1;
	String wrong_alg = cose_raw; wrong_alg[4] = 0x27;
	String wrong_cose_curve = cose_raw; wrong_cose_curve[6] = 2;
	String missing_label = cose_raw; missing_label[0] = 0xa4; missing_label.resize(missing_label.size() - 35);
	String short_x = String("\xa5\x01\x02\x03\x26\x20\x01\x21\x58\x1f",10) + String(31, 'x') + String("\x22\x58\x20", 3) + String(32, 'y');
	String duplicate_label = cose_raw; duplicate_label[0] = 0xa6; duplicate_label += String("\x01\x02", 2);
	String invalid_point = String("\xa5\x01\x02\x03\x26\x20\x01\x21\x58\x20", 10) + String(32, 'x') + String("\x22\x58\x20", 3) + String(32, 'y');
	String der = b64url_decode(signature);
	String tampered_der = der; tampered_der[10] ^= 1;
	String trailing_der = der + String("\x00", 1);
	String noncanonical_der = der; noncanonical_der[1]++; noncanonical_der[3]++; noncanonical_der.insert(4, 1, '\0');
	DValue valid_verify = verify_cose(cose, message, signature);
	DValue tampered_signature = verify_cose(cose, message, b64url_encode(tampered_der));
	DValue tampered_message = verify_cose(cose, b64url_encode("tampered"), signature);
	DValue malformed_der = verify_cose(cose, message, b64url_encode(String("\x30\x00", 2)));
	DValue noncanonical_der_result = verify_cose(cose, message, b64url_encode(noncanonical_der));
	DValue trailing_der_result = verify_cose(cose, message, b64url_encode(trailing_der));
	DValue invalid_point_result = verify_cose(b64url_encode(invalid_point), message, signature);
	String large_cbor = String("\x58\x81", 2) + String(129, 'x');
	String oversized_cbor = String("\x5a\x00\x00\x40\x00", 5) + String(16384, 'x');
	String duplicate_compound = String("\xa2\x82\x41[\x41]\x01\x82\x41[\x41]\x02", 13);
	String distinct_compound = String("\xa2\x82\x41[\x41]\x01\x82\x41[\x41[\x02", 13);
	String node_overflow = String("\x99\x01\x00", 3) + String(256, 0);
	String decoded;
	bool cbor_valid = decode_cbor(String("\x82\x01\x62ok", 5))["ok"].to_bool();
	bool cbor_large = decode_cbor(large_cbor)["ok"].to_bool();
	bool cbor_control_text = decode_cbor(String("\x61\x01", 2))["ok"].to_bool();
	bool cbor_duplicate = cbor_error(String("\xa2\x01\x02\x01\x03", 5));
	bool cbor_compound_duplicate = cbor_error(duplicate_compound);
	bool cbor_compound_distinct = decode_cbor(distinct_compound)["ok"].to_bool();
	bool cbor_invalid_utf8 = cbor_error(String("\x61\x80", 2));
	bool cbor_truncated = cbor_error(String("\xa1", 1));
	bool cbor_trailing = cbor_error(String("\x01\x02", 2));
	bool cbor_depth = cbor_error(String(17, '\x81') + "\x00");
	bool cbor_nodes = cbor_error(node_overflow);
	bool cbor_size = cbor_error(oversized_cbor);
	bool cbor_indefinite = cbor_error(String("\x9f\x01\xff", 3));
	bool cbor_integer_overflow = cbor_error(String("\x5b\xff\xff\xff\xff\xff\xff\xff\xff", 9));
	bool cbor_nonminimal = cbor_error(String("\x18\x17", 2));
	bool b64_padding = !uce_base64url_decode(cose + "=", decoded, UCE_CBOR_MAX_BASE64URL);
	bool b64_truncated = !uce_base64url_decode("A", decoded, UCE_CBOR_MAX_BASE64URL);
	bool b64_trailing_bits = !uce_base64url_decode("AB", decoded, UCE_CBOR_MAX_BASE64URL);
	bool cose_valid = parse(cose)["ok"].to_bool();
	bool cose_wrong_kty = cose_error(b64url_encode(wrong_kty));
	bool cose_wrong_alg = cose_error(b64url_encode(wrong_alg));
	bool cose_wrong_curve = cose_error(b64url_encode(wrong_cose_curve));
	bool cose_short_coordinate = cose_error(b64url_encode(short_x));
	bool cose_missing_label = cose_error(b64url_encode(missing_label));
	bool cose_duplicate_label = cose_error(b64url_encode(duplicate_label));
	bool cose_invalid_point = invalid_point_result["error"].to_string() == "invalid_key_or_payload";
	bool verify_valid = valid_verify["ok"].to_bool() && valid_verify["valid"].to_bool();
	bool verify_tampered_signature = tampered_signature["ok"].to_bool() && !tampered_signature["valid"].to_bool();
	bool verify_tampered_message = tampered_message["ok"].to_bool() && !tampered_message["valid"].to_bool();
	bool verify_malformed_der = malformed_der["error"].to_string() == "invalid_signature";
	bool verify_noncanonical_der = noncanonical_der_result["error"].to_string() == "invalid_signature";
	bool verify_trailing_der = trailing_der_result["error"].to_string() == "invalid_signature";
	bool cbor_ok = cbor_valid && cbor_large && cbor_control_text && cbor_duplicate && cbor_compound_duplicate && cbor_compound_distinct && cbor_invalid_utf8 && cbor_truncated && cbor_trailing && cbor_depth && cbor_nodes && cbor_size && cbor_indefinite && cbor_integer_overflow && cbor_nonminimal;
	bool cose_ok = cose_valid && cose_wrong_kty && cose_wrong_alg && cose_wrong_curve && cose_short_coordinate && cose_missing_label && cose_duplicate_label && cose_invalid_point;
	bool cose_negative = verify_valid && verify_tampered_signature && verify_tampered_message && verify_malformed_der && verify_noncanonical_der && verify_trailing_der;
	bool b64_ok = b64_padding && b64_truncated && b64_trailing_bits;
	bool negatives_ok = !sign(wrong_curve)["ok"].to_bool() && !sign(malformed)["ok"].to_bool() && !sign(mismatch)["ok"].to_bool() && crypto_operation_native(unsupported)["error"].to_string() == "unsupported_operation" && crypto_operation_native(unknown_algorithm)["error"].to_string() == "unsupported_algorithm" && crypto_operation_native(untyped_algorithm)["error"].to_string() == "invalid_request" && crypto_operation_native(list_request)["error"].to_string() == "invalid_key_or_payload" && crypto_operation_native(control_request)["error"].to_string() == "invalid_request" && crypto_operation_native(oversized)["error"].to_string() == "invalid_request" && crypto_operation_native(nonfinite)["error"].to_string() == "invalid_request";
	if(!(kid_ok && signed_ok && tamper_ok && negatives_ok && cose_ok && cbor_ok && cose_negative && b64_ok)) std::cerr << "kid=" << kid_ok << " signed=" << signed_ok << " tamper=" << tamper_ok << " negatives=" << negatives_ok << " cbor=" << cbor_ok << " cose=" << cose_ok << " verify=" << cose_negative << " b64=" << b64_ok << " large=" << cbor_large << " compound=" << cbor_compound_duplicate << "/" << cbor_compound_distinct << " der=" << verify_malformed_der << "/" << verify_noncanonical_der << "/" << verify_trailing_der << " cborparts=" << cbor_valid << cbor_control_text << cbor_duplicate << cbor_invalid_utf8 << cbor_truncated << cbor_trailing << cbor_depth << cbor_nodes << cbor_size << cbor_indefinite << cbor_integer_overflow << cbor_nonminimal << "\\n";
	return(kid_ok && signed_ok && tamper_ok && negatives_ok && cose_ok && cbor_ok && cose_negative && b64_ok ? 0 : 1);
}
EOF
"${CXX:-c++}" -std=c++20 -fpermissive -I. "$test_source" -lpcre2-8 -lcrypto -o "$test_binary"
"$test_binary"
echo "native structured crypto operation passed"
