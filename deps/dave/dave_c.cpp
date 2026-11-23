#include "dave_c.h"
#include <dave/dave.h>
#include <dave/dave_interfaces.h>
#include <mls/crypto.h>
#include <mls/messages.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "mls_key_ratchet.h"   /* libdave/cpp/src (test helper only): the C API's key-ratchet handle points at one */

struct dave_session   { DAVESessionHandle h; };
struct dave_encryptor { DAVEEncryptorHandle h; };
struct dave_decryptor { DAVEDecryptorHandle h; };

extern "C" {

void dave_free(void *p) { daveFree(p); }
uint16_t dave_max_protocol_version(void) { return daveMaxSupportedProtocolVersion(); }
void dave_set_log_callback(void (*cb)(int, const char *, int, const char *)) {
    daveSetLogSinkCallback(reinterpret_cast<DAVELogSinkCallback>(cb));
}

dave_session_t *dave_session_new(dave_fail_cb cb, void *user) {
    DAVESessionHandle h = daveSessionCreate(nullptr, nullptr, cb, user);
    if (!h) return nullptr;
    dave_session_t *s = (dave_session_t *)std::malloc(sizeof(*s));
    s->h = h;
    return s;
}
void dave_session_free(dave_session_t *s) { if (s) { daveSessionDestroy(s->h); std::free(s); } }
void dave_session_init(dave_session_t *s, uint16_t v, uint64_t ch, const char *uid) { daveSessionInit(s->h, v, ch, uid); }
void dave_session_reset(dave_session_t *s) { daveSessionReset(s->h); }
void dave_session_set_protocol_version(dave_session_t *s, uint16_t v) { daveSessionSetProtocolVersion(s->h, v); }
uint16_t dave_session_get_protocol_version(dave_session_t *s) { return daveSessionGetProtocolVersion(s->h); }
void dave_session_set_external_sender(dave_session_t *s, const uint8_t *d, size_t n) { daveSessionSetExternalSender(s->h, d, n); }

int dave_session_get_key_package(dave_session_t *s, uint8_t **out, size_t *n) {
    *out = nullptr; *n = 0;
    daveSessionGetMarshalledKeyPackage(s->h, out, n);
    return (*out && *n) ? 0 : -1;
}

int dave_session_process_proposals(dave_session_t *s, const uint8_t *d, size_t n,
                                   const char **ids, size_t n_ids, uint8_t **out, size_t *out_n) {
    *out = nullptr; *out_n = 0;
    daveSessionProcessProposals(s->h, d, n, ids, n_ids, out, out_n);
    return 0;
}

int dave_session_process_commit(dave_session_t *s, const uint8_t *d, size_t n, uint64_t **roster, size_t *nr) {
    if (roster) *roster = nullptr;
    if (nr) *nr = 0;
    DAVECommitResultHandle r = daveSessionProcessCommit(s->h, d, n);
    if (!r) return 1;
    int rc = 0;
    if (daveCommitResultIsFailed(r)) rc = 1;
    else if (daveCommitResultIsIgnored(r)) rc = 2;
    else if (roster && nr) daveCommitResultGetRosterMemberIds(r, roster, nr);
    daveCommitResultDestroy(r);
    return rc;
}

int dave_session_process_welcome(dave_session_t *s, const uint8_t *d, size_t n, const char **ids,
                                 size_t n_ids, uint64_t **roster, size_t *nr) {
    if (roster) *roster = nullptr;
    if (nr) *nr = 0;
    DAVEWelcomeResultHandle r = daveSessionProcessWelcome(s->h, d, n, ids, n_ids);
    if (!r) return 1;
    if (roster && nr) daveWelcomeResultGetRosterMemberIds(r, roster, nr);
    daveWelcomeResultDestroy(r);
    return 0;
}

dave_encryptor_t *dave_encryptor_new(void) {
    DAVEEncryptorHandle h = daveEncryptorCreate();
    if (!h) return nullptr;
    dave_encryptor_t *e = (dave_encryptor_t *)std::malloc(sizeof(*e));
    e->h = h;
    return e;
}
void dave_encryptor_free(dave_encryptor_t *e) { if (e) { daveEncryptorDestroy(e->h); std::free(e); } }
int dave_encryptor_set_ratchet(dave_encryptor_t *e, dave_session_t *s, const char *uid) {
    DAVEKeyRatchetHandle k = daveSessionGetKeyRatchet(s->h, uid);
    if (!k) return -1;
    daveEncryptorSetKeyRatchet(e->h, k);
    daveKeyRatchetDestroy(k);
    return 0;
}
void dave_encryptor_set_passthrough(dave_encryptor_t *e, int on) { daveEncryptorSetPassthroughMode(e->h, on != 0); }
int dave_encryptor_has_ratchet(dave_encryptor_t *e) { return daveEncryptorHasKeyRatchet(e->h); }
size_t dave_encryptor_max_size(dave_encryptor_t *e, size_t n) {
    return daveEncryptorGetMaxCiphertextByteSize(e->h, DAVE_MEDIA_TYPE_AUDIO, n);
}
int dave_encrypt_opus(dave_encryptor_t *e, uint32_t ssrc, const uint8_t *in, size_t n,
                      uint8_t *out, size_t cap, size_t *w) {
    daveEncryptorAssignSsrcToCodec(e->h, ssrc, DAVE_CODEC_OPUS);
    return (int)daveEncryptorEncrypt(e->h, DAVE_MEDIA_TYPE_AUDIO, ssrc, in, n, out, cap, w);
}

dave_decryptor_t *dave_decryptor_new(void) {
    DAVEDecryptorHandle h = daveDecryptorCreate();
    if (!h) return nullptr;
    dave_decryptor_t *d = (dave_decryptor_t *)std::malloc(sizeof(*d));
    d->h = h;
    return d;
}
void dave_decryptor_free(dave_decryptor_t *d) { if (d) { daveDecryptorDestroy(d->h); std::free(d); } }
int dave_decryptor_set_ratchet(dave_decryptor_t *d, dave_session_t *s, const char *uid) {
    DAVEKeyRatchetHandle k = daveSessionGetKeyRatchet(s->h, uid);
    if (!k) return -1;
    daveDecryptorTransitionToKeyRatchet(d->h, k);
    daveKeyRatchetDestroy(k);
    return 0;
}
void dave_decryptor_set_passthrough(dave_decryptor_t *d, int on) { daveDecryptorTransitionToPassthroughMode(d->h, on != 0); }
size_t dave_decryptor_max_size(dave_decryptor_t *d, size_t n) {
    return daveDecryptorGetMaxPlaintextByteSize(d->h, DAVE_MEDIA_TYPE_AUDIO, n);
}
int dave_decrypt_opus(dave_decryptor_t *d, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w) {
    return (int)daveDecryptorDecrypt(d->h, DAVE_MEDIA_TYPE_AUDIO, in, n, out, cap, w);
}

/* ---- video ---- */
size_t dave_encryptor_max_size_video(dave_encryptor_t *e, size_t n) {
    return daveEncryptorGetMaxCiphertextByteSize(e->h, DAVE_MEDIA_TYPE_VIDEO, n);
}
int dave_encrypt_video(dave_encryptor_t *e, uint32_t ssrc, int codec, const uint8_t *in, size_t n,
                       uint8_t *out, size_t cap, size_t *w) {
    *w = 0;
    if (codec < DAVE_CODEC_VP8 || codec > DAVE_CODEC_AV1) return -1;
    if (cap < daveEncryptorGetMaxCiphertextByteSize(e->h, DAVE_MEDIA_TYPE_VIDEO, n)) return -2;
    daveEncryptorAssignSsrcToCodec(e->h, ssrc, (DAVECodec)codec);
    return (int)daveEncryptorEncrypt(e->h, DAVE_MEDIA_TYPE_VIDEO, ssrc, in, n, out, cap, w);
}
size_t dave_decryptor_max_size_video(dave_decryptor_t *d, size_t n) {
    return daveDecryptorGetMaxPlaintextByteSize(d->h, DAVE_MEDIA_TYPE_VIDEO, n);
}
int dave_decrypt_video(dave_decryptor_t *d, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w) {
    *w = 0;
    if (cap < daveDecryptorGetMaxPlaintextByteSize(d->h, DAVE_MEDIA_TYPE_VIDEO, n)) return -2;
    return (int)daveDecryptorDecrypt(d->h, DAVE_MEDIA_TYPE_VIDEO, in, n, out, cap, w);
}
int dave_frame_looks_encrypted(const uint8_t *in, size_t n) { return n >= 2 && in[n - 1] == 0xFA && in[n - 2] == 0xFA; }

/* ---- test helpers ---- */
int dave_test_get_key(dave_session_t *s, const char *uid, uint32_t gen, uint8_t key[16]) {
    DAVEKeyRatchetHandle k = daveSessionGetKeyRatchet(s->h, uid);
    if (!k) return -1;
    /* the C API's ratchet handle is a discord::dave::MlsKeyRatchet (bindings_capi.cpp casts it the same way) */
    discord::dave::IKeyRatchet *r = reinterpret_cast<discord::dave::MlsKeyRatchet *>(k);
    auto kb = r->GetKey(gen);
    int rc = -2;
    if (kb.size() >= 16) { std::memcpy(key, kb.data(), 16); rc = 0; }
    daveKeyRatchetDestroy(k);
    return rc;
}
int dave_test_make_pair(uint64_t gid, const char *ua, const char *ub, dave_session_t **pa, dave_session_t **pb) {
    using namespace mlspp;
    *pa = *pb = nullptr;
    try {
        CipherSuite suite(CipherSuite::ID::P256_AES128GCM_SHA256_P256);
        bytes groupId; for (int i = 7; i >= 0; --i) groupId.push_back((uint8_t)(gid >> (i * 8)));
        auto esig = SignaturePrivateKey::generate(suite);
        ExternalSender es; es.signature_key = esig.public_key; es.credential = Credential::basic({0x00, 0x01, 0x01, 0x00});
        auto esb = tls::marshal(es);
        dave_session_t *A = dave_session_new(nullptr, nullptr), *B = dave_session_new(nullptr, nullptr);
        if (!A || !B) { dave_session_free(A); dave_session_free(B); return 1; }
        dave_session_set_external_sender(A, esb.data(), esb.size()); dave_session_set_external_sender(B, esb.data(), esb.size());
        dave_session_init(A, 1, gid, ua); dave_session_init(B, 1, gid, ub);
        uint8_t *kpa = nullptr, *kpb = nullptr; size_t kna = 0, knb = 0;
        if (dave_session_get_key_package(A, &kpa, &kna) || dave_session_get_key_package(B, &kpb, &knb)) { dave_session_free(A); dave_session_free(B); return 2; }
        auto prop = Proposal{Add{tls::get<KeyPackage>(bytes(std::vector<uint8_t>(kpb, kpb + knb)))}};
        dave_free(kpa); dave_free(kpb);
        auto msg = external_proposal(suite, groupId, 0, prop, 0, esig);
        tls::ostream po; po << false; po << std::vector<MLSMessage>{msg}; auto pbytes = po.bytes();
        const char *ids[] = {ua, ub}; uint8_t *cw = nullptr; size_t cwn = 0;
        dave_session_process_proposals(A, pbytes.data(), pbytes.size(), ids, 2, &cw, &cwn);
        if (!cw || !cwn) { dave_session_free(A); dave_session_free(B); return 3; }
        bytes cwb(std::vector<uint8_t>(cw, cw + cwn)); dave_free(cw);
        tls::istream in(cwb); MLSMessage cm; Welcome wm; in >> cm; in >> wm;
        auto cb = tls::marshal(cm), wb = tls::marshal(wm);
        uint64_t *ro = nullptr; size_t rn = 0;
        int crc = dave_session_process_commit(A, cb.data(), cb.size(), &ro, &rn); if (ro) dave_free(ro); ro = nullptr;
        int wrc = dave_session_process_welcome(B, wb.data(), wb.size(), ids, 2, &ro, &rn); if (ro) dave_free(ro);
        if (crc || wrc) { dave_session_free(A); dave_session_free(B); return 4; }
        *pa = A; *pb = B; return 0;
    } catch (...) { return 9; }
}

} /* extern "C" */
