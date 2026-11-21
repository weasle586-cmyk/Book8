#include "smb.h"
#include "ntlm.h"
#include "sha256.h"
#include "../include/string.h"
#include "../include/stdio.h"

enum { CONNECT = 1, NEGOTIATE, SESSION, AUTH, TREE, OPEN_DIR, QUERY, CLOSE_DIR,
    OPEN_FILE, READ_FILE, CLOSE_FILE, OPEN_UPLOAD, WRITE_FILE, CLOSE_UPLOAD };
static uint64_t auth_nonce;
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8u)); }
static uint32_t u32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u); }
static uint64_t u64(const uint8_t *p) { return u32(p) | ((uint64_t)u32(p + 4u) << 32u); }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u); }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2u, (uint16_t)(v >> 16u)); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4u, (uint32_t)(v >> 32u)); }
static bool fail(struct book8_share *s, const char *message)
{
    snprintf(s->error, sizeof(s->error), "%s", message);
    s->state = SHARE_FAILED;
    book8_tcp_close(&s->tcp);
    book8_ntlm_clear(s->password, sizeof(s->password));
    book8_ntlm_clear(s->auth_token, sizeof(s->auth_token));
    book8_ntlm_clear(s->session_key, sizeof(s->session_key));
    book8_ntlm_clear(s->request, sizeof(s->request));
    book8_ntlm_clear(s->tcp.tx, sizeof(s->tcp.tx));
    return false;
}
static bool component(const char *name, uint32_t limit)
{
    uint32_t n = 0u;
    while (name && name[n] && n < limit) {
        unsigned char c = (unsigned char)name[n++];
        if (c < 32u || c >= 127u || c == '\\' || c == '/' || c == ':'
            || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') { return false; }
    }
    return name && n && n < limit && strcmp(name, ".") && strcmp(name, "..")
        && name[n - 1u] != '.' && name[n - 1u] != ' ';
}
static bool path_ok(const char *path)
{
    if (!path) { return false; }
    char part[256]; uint32_t n = 0u, total = 0u;
    for (;;) {
        char c = path[total++];
        if (total > 256u) { return false; }
        if (!c || c == '\\') {
            part[n] = '\0';
            if (n && !component(part, sizeof(part))) { return false; }
            if (!n && (c || total != 1u)) { return false; }
            n = 0u;
            if (!c) { return true; }
        } else { part[n++] = c; }
    }
}
static uint32_t utf16(uint8_t *out, const char *in)
{
    uint32_t n = 0u;
    while (*in) { out[n++] = (uint8_t)*in++; out[n++] = 0u; }
    return n;
}
static uint8_t *header(struct book8_share *s, uint16_t command, uint32_t body)
{
    memset(s->request, 0u, sizeof(s->request));
    uint8_t *p = s->request + 4u;
    memcpy(p, "\xfeSMB", 4u); p16(p + 4u, 64u);
    p16(p + 12u, command); p16(p + 14u, 1u);
    s->pending_id = s->message_id++;
    p64(p + 24u, s->pending_id); p32(p + 32u, 1u);
    p32(p + 36u, s->tree); p64(p + 40u, s->session);
    s->command = command; s->request_length = body + 68u;
    return p + 64u;
}
static uint32_t der_size(uint32_t length) { return length < 128u ? 2u : length < 256u ? 3u : 4u; }
static uint32_t der_header(uint8_t *out, uint8_t tag, uint32_t length)
{
    out[0] = tag;
    if (length < 128u) { out[1] = (uint8_t)length; }
    else if (length < 256u) { out[1] = 0x81u; out[2] = (uint8_t)length; }
    else { out[1] = 0x82u; out[2] = (uint8_t)(length >> 8u); out[3] = (uint8_t)length; }
    return der_size(length);
}
static void session_request(struct book8_share *s, bool auth)
{
    uint8_t token[96] = {0};
    memcpy(token, "NTLMSSP", 7u); p32(token + 8u, auth ? 3u : 1u);
    uint32_t size = auth ? 65u : 32u;
    if (auth) {
        /* Explicit anonymous NTLM: no password, response, or session key. */
        for (uint32_t i = 12u; i < 60u; i += 8u) { p32(token + i + 4u, 64u); }
        p16(token + 12u, 1u); p16(token + 14u, 1u);
        p32(token + 60u, 0x00088a01u);
    } else { p32(token + 12u, s->credentials ? 0x00888215u : 0x00088207u); }
    uint8_t *b = header(s, 1u, 24u);
    p16(b, 25u); b[3] = 1u; p16(b + 12u, 88u);
    uint8_t *blob = b + 24u;
    if (!auth) {
        /* SPNEGO NegTokenInit containing the NTLM mechanism and token. */
        static const uint8_t prefix[] = {0x60,0,0x06,0x06,0x2b,0x06,0x01,0x05,0x05,0x02,
            0xa0,0,0x30,0,0xa0,0x0e,0x30,0x0c,0x06,0x0a,0x2b,0x06,0x01,0x04,0x01,
            0x82,0x37,0x02,0x02,0x0a,0xa2,0,0x04,0};
        memcpy(blob, prefix, sizeof(prefix));
        blob[1] = (uint8_t)(32u + size); blob[11] = (uint8_t)(22u + size);
        blob[13] = (uint8_t)(20u + size); blob[31] = (uint8_t)(size + 2u);
        blob[33] = (uint8_t)size; memcpy(blob + 34u, token, size); size += 34u;
    } else if (s->credentials) {
        /* DER lengths may exceed the short-form 127-byte encoding. */
        uint32_t n = s->auth_size, octet = n + der_size(n);
        uint32_t tagged = octet + der_size(octet), sequence = tagged + der_size(tagged);
        uint32_t pos = der_header(blob, 0xa1u, sequence);
        pos += der_header(blob + pos, 0x30u, tagged); pos += der_header(blob + pos, 0xa2u, octet);
        pos += der_header(blob + pos, 4u, n);
        memcpy(blob + pos, s->auth_token, n); size = n + pos;
        book8_ntlm_clear(s->auth_token, sizeof(s->auth_token));
    } else {
        uint8_t prefix[] = {0xa1, (uint8_t)(size + 6u), 0x30, (uint8_t)(size + 4u),
            0xa2, (uint8_t)(size + 2u), 0x04, (uint8_t)size};
        memcpy(blob, prefix, sizeof(prefix)); memcpy(blob + 8u, token, size); size += 8u;
    }
    p16(b + 14u, (uint16_t)size); s->request_length += size;
}
static void make_request(struct book8_share *s)
{
    uint8_t *b;
    switch (s->stage) {
    case NEGOTIATE:
        b = header(s, 0u, 40u); p16(b, 36u); p16(b + 2u, 2u); p16(b + 4u, 1u);
        memcpy(b + 12u, "BOOK8-NETSHARE-01", 16u);
        p16(b + 36u, 0x0202u); p16(b + 38u, 0x0210u); break;
    case SESSION: session_request(s, false); break;
    case AUTH: session_request(s, true); break;
    case TREE: {
        char unc[96];
        snprintf(unc, sizeof(unc), "\\\\%u.%u.%u.%u\\%s", s->tcp.peer >> 24u,
            (s->tcp.peer >> 16u) & 255u, (s->tcp.peer >> 8u) & 255u, s->tcp.peer & 255u, s->share);
        b = header(s, 3u, 8u); p16(b, 9u); p16(b + 4u, 72u);
        uint32_t size = utf16(b + 8u, unc);
        p16(b + 6u, (uint16_t)size); s->request_length += size; break;
    }
    case OPEN_DIR: case OPEN_FILE: case OPEN_UPLOAD: {
        b = header(s, 5u, 56u); p16(b, 57u); p32(b + 4u, 2u);
        p32(b + 24u, s->stage == OPEN_UPLOAD ? 0x00100102u : 0x00100081u);
        p32(b + 32u, 7u); p32(b + 36u, s->stage == OPEN_UPLOAD ? 2u : 1u);
        p32(b + 40u, s->stage == OPEN_DIR ? 1u : 0x40u);
        const char *name = s->stage == OPEN_DIR ? s->path : s->file;
        uint32_t size = utf16(b + 56u, name);
        p16(b + 44u, 120u); p16(b + 46u, (uint16_t)size);
        s->request_length += size ? size : 1u; break;
    }
    case QUERY:
        b = header(s, 14u, 34u); p16(b, 33u); b[2] = 1u;
        memcpy(b + 8u, s->file_id, 16u); p16(b + 24u, 96u); p16(b + 26u, 2u);
        p32(b + 28u, 8192u); b[32] = '*'; break;
    case READ_FILE:
        b = header(s, 8u, 49u); p16(b, 49u);
        s->read_size = s->expected - s->downloaded;
        if (s->read_size > 8192u) { s->read_size = 8192u; }
        p32(b + 4u, s->read_size); p64(b + 8u, s->downloaded);
        memcpy(b + 16u, s->file_id, 16u); break;
    case WRITE_FILE:
        s->read_size = s->expected - s->downloaded;
        if (s->read_size > 1024u) { s->read_size = 1024u; }
        if (s->read_size > s->max_write) { s->read_size = s->max_write; }
        b = header(s, 9u, 48u + s->read_size); p16(b, 49u); p16(b + 2u, 112u);
        p32(b + 4u, s->read_size); p64(b + 8u, s->downloaded);
        memcpy(b + 16u, s->file_id, 16u); memcpy(b + 48u, s->data + s->downloaded, s->read_size);
        break;
    default:
        b = header(s, 6u, 24u); p16(b, 24u); memcpy(b + 8u, s->file_id, 16u); break;
    }
    uint32_t bytes = s->request_length - 4u;
    s->request[1] = (uint8_t)(bytes >> 16u); s->request[2] = (uint8_t)(bytes >> 8u);
    s->request[3] = (uint8_t)bytes;
    if (s->signing && s->authenticated) {
        uint8_t mac[32]; p32(s->request + 20u, 8u);
        book8_hmac_sha256(s->session_key, 16u, s->request + 4u, bytes, mac);
        memcpy(s->request + 52u, mac, 16u); book8_ntlm_clear(mac, sizeof(mac));
    }
}
static bool directory_reply(struct book8_share *s, const uint8_t *p, uint32_t size)
{
    if (size < 72u || u16(p + 64u) != 9u) { return fail(s, "Malformed SMB directory response."); }
    uint32_t offset = u16(p + 66u), bytes = u32(p + 68u);
    if (offset < 72u || offset > size || bytes > size - offset) { return fail(s, "Invalid SMB directory bounds."); }
    p += offset;
    while (bytes) {
        if (bytes < 64u) { return fail(s, "Truncated remote directory entry."); }
        uint32_t next = u32(p), length = u32(p + 60u);
        if ((length & 1u) || length > 510u || length > bytes - 64u
            || (next && (next < 64u + length || next > bytes || (next & 7u)))) {
            return fail(s, "Remote directory name/record exceeds supported bounds.");
        }
        char name[256];
        for (uint32_t i = 0u; i < length / 2u; ++i) {
            if (p[64u + i * 2u + 1u] || p[64u + i * 2u] >= 127u
                || p[64u + i * 2u] < 32u) { return fail(s, "Non-ASCII remote filenames are not supported."); }
            name[i] = (char)p[64u + i * 2u];
        }
        name[length / 2u] = '\0';
        if (strcmp(name, ".") && strcmp(name, "..")) {
            if (!component(name, sizeof(name))) { return fail(s, "Unsafe remote filename rejected."); }
            if (s->count == BOOK8_SHARE_ENTRIES) { return fail(s, "Directory exceeds 128 entries; choose a smaller directory."); }
            struct book8_share_entry *entry = &s->entries[s->count++];
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            entry->size = u64(p + 40u); entry->directory = (u32(p + 56u) & 16u) != 0u;
            entry->allocated = u64(p + 48u); entry->created = u64(p + 8u);
            entry->modified = u64(p + 24u); entry->attributes = u32(p + 56u);
        }
        if (!next) { break; }
        p += next; bytes -= next;
    }
    return true;
}
static bool response(struct book8_share *s, uint8_t *p, uint32_t size)
{
    if (size < 64u || memcmp(p, "\xfeSMB", 4u) || u16(p + 4u) != 64u
        || u16(p + 12u) != s->command || u64(p + 24u) != s->pending_id
        || !(u32(p + 16u) & 1u) || (u32(p + 16u) & 2u)
        || u32(p + 20u) != 0u) { return fail(s, "Malformed, asynchronous or unexpected SMB response."); }
    s->status = u32(p + 8u);
    if ((u32(p + 16u) & 8u) && !s->signing) { return fail(s, "Unexpected signed SMB response without a signing session."); }
    if (s->signing && (s->authenticated || (s->stage == AUTH && !s->status && (u32(p + 16u) & 8u)))) {
        uint8_t signature[16], mac[32];
        if (!(u32(p + 16u) & 8u)) { return fail(s, "Server returned an unsigned SMB response."); }
        memcpy(signature, p + 48u, 16u); memset(p + 48u, 0u, 16u);
        book8_hmac_sha256(s->session_key, 16u, p, size, mac); memcpy(p + 48u, signature, 16u);
        bool valid = book8_digest_equal(signature, mac, 16u);
        book8_ntlm_clear(mac, sizeof(mac));
        if (!valid) { return fail(s, "SMB response signature verification failed."); }
    }
    if (s->stage == SESSION && s->status == 0xc0000016u) {
        if (size < 72u || u16(p + 64u) != 9u) { return fail(s, "Malformed SMB session challenge."); }
        uint32_t offset = u16(p + 68u), bytes = u16(p + 70u);
        if (offset < 72u || offset > size || bytes > size - offset) { return fail(s, "Invalid session challenge bounds."); }
        bool ntlm = false;
        for (uint32_t i = 0u; i + 12u <= bytes; ++i) {
            if (!memcmp(p + offset + i, "NTLMSSP\0", 8u) && u32(p + offset + i + 8u) == 2u) {
                ntlm = true;
                if (s->credentials && !book8_ntlm_auth(s->username, s->domain, s->password,
                    p + offset + i, bytes - i, ++auth_nonce + s->deadline, s->auth_token, &s->auth_size, s->session_key)) {
                    return fail(s, "Unsupported NTLM challenge or server MIC/channel-binding policy.");
                }
                break;
            }
        }
        if (!ntlm) { return fail(s, "Server does not offer NTLM authentication."); }
        book8_ntlm_clear(s->password, sizeof(s->password));
        s->session = u64(p + 40u); s->stage = AUTH; return true;
    }
    if (s->stage == QUERY && s->status == 0x80000006u) { s->stage = CLOSE_DIR; return true; }
    if (s->status) {
        s->auth_failed = s->stage == SESSION || s->stage == AUTH;
        if (s->auth_failed) { return fail(s, "Login failed. Check username, domain and password, or try Guest."); }
        if (s->status == 0xc0000022u) { return fail(s, "Access denied. Check this account's share/file permissions."); }
        if (s->status == 0xc0000035u) { return fail(s, "Destination already exists; uploads never overwrite files."); }
        if (s->status == 0xc00000ccu) { return fail(s, "Share not found. Check the server profile's share name."); }
        if (s->status == 0xc0000034u) { return fail(s, "File or folder not found. Refresh the directory."); }
        if (s->uploading && s->stage != OPEN_UPLOAD) { return fail(s, "Upload failed. A partial file may remain on the server; check it before retrying."); }
        return fail(s, "The server could not complete the file operation. Check availability and permissions.");
    }
    if (s->stage >= TREE && u64(p + 40u) != s->session) { return fail(s, "SMB session mismatch."); }
    if (s->stage > TREE && u32(p + 36u) != s->tree) { return fail(s, "SMB tree mismatch."); }
    switch (s->stage) {
    case NEGOTIATE:
        if (size < 128u || u16(p + 64u) != 65u
            || (u16(p + 68u) != 0x0202u && u16(p + 68u) != 0x0210u)) {
            return fail(s, "Server must support SMB2.0.2 or SMB2.1.");
        }
        if ((u16(p + 66u) & 2u) && !s->credentials) { return fail(s, "Server requires signing. Connect with a username and password."); }
        s->signing = s->credentials && (u16(p + 66u) & 1u);
        s->max_write = u32(p + 100u);
        if (u32(p + 96u) < 8192u || u32(p + 92u) < 8192u) { return fail(s, "Server SMB read/transaction limits are too small."); }
        s->stage = SESSION; break;
    case SESSION: case AUTH:
        if (s->credentials && s->stage != AUTH) { return fail(s, "Server skipped the NTLM challenge; refusing an unverified login."); }
        if (size < 72u || u16(p + 64u) != 9u || (u16(p + 66u) & 4u)) {
            return fail(s, "Malformed or encrypted SMB session; encryption is not supported.");
        }
        if (s->credentials && (u16(p + 66u) & 3u)) {
            s->auth_failed = true; return fail(s, "Server mapped this login to Guest. Check credentials or choose Guest explicitly.");
        }
        if (!s->credentials && !(u16(p + 66u) & 3u)) { return fail(s, "Server did not grant a guest/null session."); }
        book8_ntlm_clear(s->password, sizeof(s->password));
        book8_ntlm_clear(s->request, sizeof(s->request));
        s->session = u64(p + 40u); s->authenticated = true; s->stage = TREE; break;
    case TREE:
        if (size < 80u || u16(p + 64u) != 16u || p[66] != 1u || (u32(p + 68u) & 0x8000u)) {
            return fail(s, "Share must be an unencrypted SMB disk tree.");
        }
        s->tree = u32(p + 36u); s->stage = OPEN_DIR; break;
    case OPEN_DIR: case OPEN_FILE: case OPEN_UPLOAD:
        if (size < 152u || u16(p + 64u) != 89u) { return fail(s, "Malformed SMB open response."); }
        memcpy(s->file_id, p + 128u, 16u);
        if (s->stage == OPEN_FILE) {
            uint64_t length = u64(p + 112u);
            if (length > BOOK8_SHARE_DOWNLOAD) { return fail(s, "Download exceeds the 1 MiB limit."); }
            s->expected = (uint32_t)length; s->downloaded = 0u;
            s->stage = length ? READ_FILE : CLOSE_FILE;
        } else if (s->stage == OPEN_UPLOAD) { s->stage = s->expected ? WRITE_FILE : CLOSE_UPLOAD; }
        else { s->stage = QUERY; }
        break;
    case QUERY:
        if (!directory_reply(s, p, size)) { return false; }
        s->stage = QUERY; break;
    case READ_FILE: {
        if (size < 80u || u16(p + 64u) != 17u) { return fail(s, "Malformed SMB read response."); }
        uint32_t offset = p[66], bytes = u32(p + 68u);
        if (offset < 80u || offset > size || bytes > size - offset || !bytes
            || bytes > s->read_size || bytes > s->expected - s->downloaded) {
            return fail(s, "Invalid/truncated SMB file data.");
        }
        memcpy(s->data + s->downloaded, p + offset, bytes); s->downloaded += bytes;
        s->stage = s->downloaded == s->expected ? CLOSE_FILE : READ_FILE; break;
    }
    case WRITE_FILE: {
        if (size < 80u || u16(p + 64u) != 17u) { return fail(s, "Malformed SMB write response; a partial remote file may remain."); }
        uint32_t bytes = u32(p + 68u);
        if (!bytes || bytes > s->read_size) { return fail(s, "Invalid SMB write count; a partial remote file may remain."); }
        s->downloaded += bytes; s->stage = s->downloaded == s->expected ? CLOSE_UPLOAD : WRITE_FILE;
        break;
    }
    default:
        if (size < 124u || u16(p + 64u) != 60u) { return fail(s, "Malformed SMB close response."); }
        s->state = s->stage == CLOSE_DIR ? SHARE_READY : s->stage == CLOSE_UPLOAD ? SHARE_UPLOADED : SHARE_DOWNLOADED;
        s->downloading = s->uploading = false; s->stage = 0u; break;
    }
    return true;
}
bool book8_share_connect(struct book8_share *s, struct book8_net *net, uint32_t ip,
    const char *share, uint64_t now)
{ return book8_share_connect_auth(s, net, ip, share, "", "", "", now); }
bool book8_share_connect_auth(struct book8_share *s, struct book8_net *net, uint32_t ip,
    const char *share, const char *username, const char *domain, const char *password, uint64_t now)
{
    if (s->tcp.net) { book8_share_close(s); }
    memset(s, 0u, sizeof(*s));
    if (!component(share, sizeof(s->share))) { return fail(s, "Invalid share name (ASCII, no path separators)."); }
    if (!username || !domain || !password || strlen(username) >= sizeof(s->username)
        || strlen(domain) >= sizeof(s->domain) || strlen(password) >= sizeof(s->password)) {
        return fail(s, "Login fields exceed the supported length.");
    }
    const char *fields[] = {username, domain, password};
    for (uint32_t i = 0; i < 3u; ++i) {
        for (const char *c = fields[i]; *c; ++c) { if ((uint8_t)*c < 32u || (uint8_t)*c >= 127u) { return fail(s, "Login fields must contain printable ASCII."); } }
    }
    snprintf(s->username, sizeof(s->username), "%s", username);
    snprintf(s->domain, sizeof(s->domain), "%s", domain);
    snprintf(s->password, sizeof(s->password), "%s", password); s->credentials = username[0] != '\0';
    snprintf(s->share, sizeof(s->share), "%s", share);
    enum book8_net_result r = book8_tcp_connect(&s->tcp, net, ip, 445u, now);
    if (r != BOOK8_NET_OK) { return fail(s, book8_net_result_string(r)); }
    s->stage = CONNECT; s->state = SHARE_BUSY; s->deadline = now + 15000u;
    return true;
}
bool book8_share_directory(struct book8_share *s, const char *path, uint64_t now)
{
    if ((s->state != SHARE_READY && s->state != SHARE_DOWNLOADED && s->state != SHARE_UPLOADED) || !s->authenticated) {
        snprintf(s->error, sizeof(s->error), "Connect before browsing remote folders."); return false;
    }
    if (!path_ok(path)) { return fail(s, "Invalid or too-long remote directory path."); }
    char copy[256]; snprintf(copy, sizeof(copy), "%s", path);
    snprintf(s->path, sizeof(s->path), "%s", copy); s->count = 0u;
    s->stage = OPEN_DIR; s->state = SHARE_BUSY; s->deadline = now + 15000u;
    return true;
}
bool book8_share_download(struct book8_share *s, uint32_t entry, uint64_t now)
{
    if ((s->state != SHARE_READY && s->state != SHARE_DOWNLOADED && s->state != SHARE_UPLOADED) || entry >= s->count
        || s->entries[entry].directory) { snprintf(s->error, sizeof(s->error), "Select a remote file to download."); return false; }
    int n = snprintf(s->file, sizeof(s->file), "%s%s%s", s->path, s->path[0] ? "\\" : "", s->entries[entry].name);
    if (n < 0 || (uint32_t)n >= sizeof(s->file)) { return fail(s, "Remote file path is too long."); }
    if (s->entries[entry].size > BOOK8_SHARE_DOWNLOAD) { return fail(s, "Download exceeds the 1 MiB limit."); }
    s->stage = OPEN_FILE; s->state = SHARE_BUSY; s->downloading = true;
    s->deadline = now + 15000u;
    return true;
}
bool book8_share_upload(struct book8_share *s, const char *name, const void *data, uint32_t size, uint64_t now)
{
    if ((s->state != SHARE_READY && s->state != SHARE_DOWNLOADED && s->state != SHARE_UPLOADED) || !s->authenticated) {
        snprintf(s->error, sizeof(s->error), "Connect before uploading a local file."); return false;
    }
    if (!component(name, 256u) || size > BOOK8_SHARE_DOWNLOAD || (!data && size)) {
        snprintf(s->error, sizeof(s->error), "Upload requires a valid filename and at most 1 MiB."); return false;
    }
    if (size && !s->max_write) { snprintf(s->error, sizeof(s->error), "Server does not support file writes."); return false; }
    int n = snprintf(s->file, sizeof(s->file), "%s%s%s", s->path, s->path[0] ? "\\" : "", name);
    if (n < 0 || (uint32_t)n >= sizeof(s->file)) { snprintf(s->error, sizeof(s->error), "Remote upload path is too long."); return false; }
    memmove(s->data, data, size); s->expected = size; s->downloaded = 0u;
    s->stage = OPEN_UPLOAD; s->state = SHARE_BUSY; s->uploading = true; s->deadline = now + 15000u;
    return true;
}
void book8_share_poll(struct book8_share *s, uint64_t now)
{
    if (s->state == SHARE_IDLE || s->state == SHARE_FAILED) { return; }
    book8_tcp_poll(&s->tcp, now);
    if (s->tcp.state == TCP_FAILED) { (void)fail(s, book8_net_result_string(s->tcp.error)); return; }
    if (s->state != SHARE_BUSY) { return; }
    if (now >= s->deadline) { (void)fail(s, "SMB response timeout (15 seconds)."); return; }
    if (s->stage == CONNECT) {
        if (s->tcp.state != TCP_CONNECTED) { return; }
        s->stage = NEGOTIATE;
    }
    if (s->waiting && s->tcp.rx_length >= 4u) {
        uint8_t *p = s->tcp.rx;
        uint32_t size = ((uint32_t)p[1] << 16u) | ((uint32_t)p[2] << 8u) | p[3];
        if (p[0] != 0u || size < 64u || size > BOOK8_TCP_RX - 4u) {
            (void)fail(s, "Invalid SMB direct-TCP frame length."); return;
        }
        if (s->tcp.rx_length < size + 4u) { return; }
        if (!response(s, p + 4u, size)) { return; }
        book8_tcp_consume(&s->tcp, size + 4u);
        s->waiting = false; s->request_length = 0u; s->deadline = now + 15000u;
    }
    if (s->state == SHARE_BUSY && !s->waiting && !s->tcp.tx_length) {
        if (!s->request_length) { make_request(s); }
        enum book8_net_result r = book8_tcp_send(&s->tcp, s->request, s->request_length, now);
        if (r == BOOK8_NET_OK) { s->waiting = true; }
        else if (r != BOOK8_NET_BUSY) { (void)fail(s, book8_net_result_string(r)); }
    }
}
void book8_share_close(struct book8_share *s)
{
    book8_tcp_close(&s->tcp); s->state = SHARE_IDLE; s->authenticated = false;
    book8_ntlm_clear(s->password, sizeof(s->password)); book8_ntlm_clear(s->session_key, sizeof(s->session_key));
    book8_ntlm_clear(s->auth_token, sizeof(s->auth_token)); book8_ntlm_clear(s->request, sizeof(s->request));
    book8_ntlm_clear(s->tcp.tx, sizeof(s->tcp.tx));
}
