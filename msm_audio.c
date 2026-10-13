/* SPDX-License-Identifier: BSD-3-Clause */
#ifdef MSM_AUDIO_GLUE_TEST
#include "tests/msm_audio_glue_stubs.h"
#else
#include "defs.h"
#endif
#include "msm_audio_capture.h"
#include <limits.h>
#include <sys/stat.h>

#if defined(ARM) && !defined(AARCH64)
#define MSM_GET_EVENT UINT32_C(0x8004610d)
#define MSM_ASYNC_READ UINT32_C(0x40046112)
#define MSM_REGISTER UINT32_C(0x40046161)
#define MSM_DEREGISTER UINT32_C(0x40046162)
#define MSM_MAP_LINE 1024
#define MSM_SESSIONS 16
struct msm_mapping { uint32_t address, length; char line[MSM_MAP_LINE]; };
struct msm_session {
    int fd;
    bool occupied, invalid;
    struct stat identity;
    uint64_t generation;
    struct msm_capture_state capture;
    struct msm_mapping maps[MSM_CAPTURE_SLOTS];
};
struct msm_request {
    unsigned int code;
    int fd;
    uint64_t generation;
    bool valid;
    uint32_t registration[2];
    struct msm_capture_aio buffer;
    struct msm_mapping mapping;
};
struct msm_map_failure { const char *reason; int error; };
struct msm_reader {
    struct tcb *tcp;
    struct msm_mapping *mapping;
    const char *failure_stage;
    struct msm_map_failure mapping_failure;
    unsigned int snapshot;
    int read_result, failure_errno;
    bool read_attempted;
};
static struct msm_session msm_sessions[MSM_SESSIONS];
static uint64_t msm_generation;
static int msm_target;
static unsigned long long msm_starttime;
static bool msm_checked, msm_dead, msm_multiple_fds;

static bool msm_identity(struct tcb *tcp)
{
    char path[64], line[1024], *tail;
    FILE *file;
    int tgid = 0;
    unsigned long long starttime = 0;
    if (!msm_checked) {
        const char *value = getenv("STRACE_MSM_AUDIO_CAPTURE_PID");
        char *end;
        long parsed = 0;
        msm_checked = true;
        if (value) {
            errno = 0; parsed = strtol(value, &end, 10);
            if (errno || !*value || *end || parsed <= 1 || parsed > INT_MAX) parsed = 0;
        }
        msm_target = (int) parsed;
    }
    if (!msm_target || msm_dead || current_wordsize != 4) return false;
    snprintf(path, sizeof(path), "/proc/%d/status", tcp->pid);
    file = fopen(path, "r");
    if (!file) return false;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "Tgid: %d", &tgid) == 1) break;
    fclose(file);
    if (tgid != msm_target) return false;
    snprintf(path, sizeof(path), "/proc/%d/stat", msm_target);
    file = fopen(path, "r");
    if (!file) return false;
    bool success = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    if (!success || !(tail = strrchr(line, ')'))) return false;
    ++tail;
    for (unsigned int field = 3; field <= 22; ++field) {
        while (*tail == ' ') ++tail;
        if (!*tail) return false;
        if (field == 22) {
            char *end;
            errno = 0; starttime = strtoull(tail, &end, 10);
            if (errno || end == tail || (*end != ' ' && *end != '\n')) return false;
            break;
        }
        while (*tail && *tail != ' ') ++tail;
    }
    if (!msm_starttime) msm_starttime = starttime;
    if (!starttime || starttime != msm_starttime) { msm_dead = true; return false; }
    return true;
}
static struct msm_session *msm_session(int fd, bool create)
{
    struct msm_session *available = NULL;
    for (size_t index = 0; index < MSM_SESSIONS; ++index) {
        struct msm_session *session = &msm_sessions[index];
        if (session->occupied && session->fd == fd) return session;
        if (!session->occupied) available = session;
    }
    if (!create || !available) return NULL;
    memset(available, 0, sizeof(*available));
    available->fd = fd; available->occupied = true;
    available->generation = ++msm_generation;
    return available;
}
void msm_audio_invalidate_fd(struct tcb *tcp, int fd)
{
    if (msm_identity(tcp)) {
        struct msm_session *session = msm_session(fd, false);
        if (session) memset(session, 0, sizeof(*session));
    }
}
static bool msm_fd_identity(struct tcb *tcp, int fd, struct stat *identity)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", tcp->pid, fd);
    return stat(path, identity) == 0 && S_ISCHR(identity->st_mode);
}
static bool msm_same_fd(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_rdev == right->st_rdev;
}
static bool msm_mapping_read(struct tcb *tcp, uint32_t address,
                             struct msm_mapping *mapping, struct msm_map_failure *failure)
{
    char path[64], line[MSM_MAP_LINE];
    unsigned long long start, end;
    char permission[5];
    bool found = false;
    struct msm_map_failure detail = {"address_unmapped", 0};
    snprintf(path, sizeof(path), "/proc/%d/maps", tcp->pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        detail = (struct msm_map_failure) {"maps_open", errno};
        if (failure) *failure = detail;
        return false;
    }
    errno = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n')) { detail.reason = "maps_line_overflow"; break; }
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permission) != 3) {
            detail.reason = "maps_line_parse"; break;
        }
        if (start <= address && address < end) {
            if (end > UINT64_C(0x100000000) || end - address > UINT32_MAX) {
                detail.reason = "mapping_span_unsupported"; break;
            }
            if (permission[0] != 'r') { detail.reason = "mapping_not_readable"; break; }
            mapping->address = address;
            mapping->length = (uint32_t) (end - address);
            memcpy(mapping->line, line, strlen(line) + 1);
            found = true; break;
        }
        errno = 0;
    }
    if (ferror(file)) detail = (struct msm_map_failure) {"maps_read", errno};
    fclose(file);
    if (failure) *failure = detail;
    return found;
}
static int msm_read_snapshot(void *opaque, uint32_t address, size_t length, void *output)
{
    struct msm_reader *reader = opaque;
    struct msm_mapping current;
    struct msm_mapping *registered = reader->mapping;
    ++reader->snapshot;
    reader->read_attempted = false;
    reader->read_result = 0;
    reader->failure_errno = 0;
    if (!msm_mapping_read(reader->tcp, registered->address, &current, &reader->mapping_failure)) {
        reader->failure_stage = "mapping_before_lookup";
        reader->failure_errno = reader->mapping_failure.error;
        return -1;
    }
    if (strcmp(current.line, registered->line)) {
        reader->failure_stage = "mapping_before_identity"; return -1;
    }
    if (address < registered->address ||
        (uint64_t) address + length > (uint64_t) registered->address + registered->length) {
        reader->failure_stage = "snapshot_span"; return -1;
    }
    reader->read_attempted = true;
    errno = 0;
    reader->read_result = umoven(reader->tcp, address, (unsigned int) length, output);
    if (reader->read_result) {
        reader->failure_errno = errno;
        reader->failure_stage = "umoven"; return -1;
    }
    if (!msm_mapping_read(reader->tcp, registered->address, &current, &reader->mapping_failure)) {
        reader->failure_stage = "mapping_after_lookup";
        reader->failure_errno = reader->mapping_failure.error;
        return -1;
    }
    if (strcmp(current.line, registered->line)) {
        reader->failure_stage = "mapping_after_identity"; return -1;
    }
    return 0;
}
static bool msm_vmflag_present(const char *flags, const char *wanted)
{
    while (*flags) {
        flags += strspn(flags, " \t");
        size_t length = strcspn(flags, " \t");
        if (length == strlen(wanted) && !strncmp(flags, wanted, length)) return true;
        flags += length;
    }
    return false;
}
static void msm_print_failure_smaps(struct tcb *tcp, const struct msm_mapping *registered)
{
    const size_t scan_limit = 4U * 1024U * 1024U;
    char path[64], line[MSM_MAP_LINE], flags[256] = {0};
    const char *reason = "mapping_missing";
    size_t scanned = 0;
    int saved_errno = 0;
    bool matched = false, observed = false;
    snprintf(path, sizeof(path), "/proc/%d/smaps", tcp->pid);
    FILE *file = fopen(path, "r");
    if (!file) { reason = "smaps_open"; saved_errno = errno; }
    else {
        for (;;) {
            unsigned long long start, end;
            char permissions[5];
            if (scanned >= scan_limit) { reason = "smaps_scan_limit"; break; }
            errno = 0;
            size_t available = scan_limit - scanned;
            int read_limit = available < sizeof(line) - 1 ? (int) available + 1 : (int) sizeof(line);
            if (!fgets(line, read_limit, file)) {
                if (ferror(file)) { reason = "smaps_read"; saved_errno = errno; }
                else if (matched) reason = "vmflags_missing";
                break;
            }
            size_t length = strlen(line);
            if (length > scan_limit - scanned) { reason = "smaps_scan_limit"; break; }
            scanned += length;
            if (!strchr(line, '\n')) {
                reason = scanned == scan_limit ? "smaps_scan_limit" : "smaps_line_overflow";
                break;
            }
            if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3) {
                if (matched) { reason = "vmflags_missing"; break; }
                if (start <= registered->address && registered->address < end) {
                    if (strcmp(line, registered->line)) { reason = "mapping_identity"; break; }
                    matched = true;
                }
                continue;
            }
            if (!matched || strncmp(line, "VmFlags:", 8)) continue;
            char *value = line + 8;
            value += strspn(value, " \t");
            value[strcspn(value, "\n")] = '\0';
            length = strlen(value);
            if (!length || length >= sizeof(flags) || strspn(value, "abcdefghijklmnopqrstuvwxyz \t") != length) {
                reason = "vmflags_invalid"; break;
            }
            memcpy(flags, value, length + 1);
            observed = true;
            break;
        }
        fclose(file);
    }
    if (observed) {
        struct msm_mapping current;
        struct msm_map_failure failure;
        if (!msm_mapping_read(tcp, registered->address, &current, &failure)) {
            observed = false; reason = "maps_after_flags_lookup"; saved_errno = failure.error;
        } else if (strcmp(current.line, registered->line)) {
            observed = false; reason = "maps_after_flags_identity";
        }
    }
    tprints(", failure_vma={");
    if (observed)
        tprintf("status=\"observed\", flags=\"%s\", io=%s, pf=%s",
                flags, msm_vmflag_present(flags, "io") ? "true" : "false",
                msm_vmflag_present(flags, "pf") ? "true" : "false");
    else tprintf("status=\"unavailable\", reason=\"%s\", errno=%d", reason, saved_errno);
    tprintf(", scanned_bytes=%zu}", scanned);
}
static void msm_print_buffer(const struct msm_capture_aio *buffer)
{
    tprintf("{address=%#x, length=%u, data_length=%u, private_data=%#x, metadata_size=%u}",
            buffer->address, buffer->length, buffer->data_length,
            buffer->private_data, buffer->metadata_size);
}
static uint32_t msm_word(const unsigned char *bytes)
{
    return (uint32_t) bytes[0] | (uint32_t) bytes[1] << 8 |
           (uint32_t) bytes[2] << 16 | (uint32_t) bytes[3] << 24;
}
static const char *msm_finish_event(struct tcb *tcp, struct msm_session *session, kernel_ulong_t arg)
{
    unsigned char event[40];
    if (umoven(tcp, arg, sizeof(event), event)) return "event_unreadable";
    uint32_t type = msm_word(event);
    tprintf("event_type=%u", type);
    if (type == 4) {
        tprintf(", codec_type=%u, channels=%u, sample_rate=%u", msm_word(event + 8),
                msm_word(event + 12), msm_word(event + 16));
        return NULL;
    }
    if (type != 3) return NULL;
    struct msm_capture_aio buffer;
    memcpy(&buffer, event + 8, sizeof(buffer));
    tprints(", buffer="); msm_print_buffer(&buffer);
    struct msm_mapping *mapping = NULL;
    for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index) {
        struct msm_mapping *candidate = &session->maps[index];
        if (candidate->length && buffer.address >= candidate->address &&
            (uint64_t) buffer.address + buffer.length <=
            (uint64_t) candidate->address + candidate->length) mapping = candidate;
    }
    if (!mapping) return "completion_mapping_missing";
    struct msm_reader reader = {.tcp = tcp, .mapping = mapping};
    struct msm_capture_result result;
    const char *error = msm_capture_complete(&session->capture, &buffer,
                                            msm_read_snapshot, &reader, &result);
    if (reader.failure_stage) {
        tprintf(", snapshot_error={snapshot=%u, stage=\"%s\", read_result=%d, errno=%d, read_attempted=%s",
                reader.snapshot, reader.failure_stage, reader.read_result,
                reader.failure_errno, reader.read_attempted ? "true" : "false");
        if (!strcmp(reader.failure_stage, "mapping_before_lookup") ||
            !strcmp(reader.failure_stage, "mapping_after_lookup"))
            tprintf(", mapping_reason=\"%s\"", reader.mapping_failure.reason);
        tprints("}");
        msm_print_failure_smaps(tcp, mapping);
    }
    if (result.bytes) {
        tprintf(", sequence=%" PRIu64 ", snapshots_equal=true, bytes=%u, frames=%u, eos=%s, contiguous=%s",
                result.sequence, result.length, result.frames, result.eos ? "true" : "false",
                result.contiguous ? "true" : "false");
        uint32_t count = result.eos ? 1 : result.frames;
        if (count > (result.length - 32) / 24) count = (result.length - 32) / 24;
        tprints(", descriptors=[");
        for (uint32_t index = 0; index < count; ++index) {
            const unsigned char *descriptor = result.bytes + 32 + index * 24;
            tprintf("%s{offset=%u,size=%u,samples=%u,ts0=%u,ts1=%u,flags=%#x}", index ? "," : "",
                    msm_word(descriptor), msm_word(descriptor+4), msm_word(descriptor+8),
                    msm_word(descriptor+12), msm_word(descriptor+16), msm_word(descriptor+20));
        }
        tprints("], raw_hex=\"");
        for (uint32_t index = 0; index < result.length; ++index) tprintf("%02x", result.bytes[index]);
        tprints("\"");
    }
    msm_capture_result_free(&result);
    return error;
}
int msm_audio_ioctl(struct tcb *tcp, unsigned int code, kernel_ulong_t arg)
{
    char path[PATH_MAX];
    struct stat identity;
    if (!msm_identity(tcp)) return -1;
    if (getfdpath(tcp, (int) tcp->u_arg[0], path, sizeof(path)) < 0 ||
        strcmp(path, "/dev/msm_aac") ||
        !msm_fd_identity(tcp, (int) tcp->u_arg[0], &identity)) return -1;
    struct msm_session *existing = msm_session((int) tcp->u_arg[0], false);
    if (existing && !msm_same_fd(&existing->identity, &identity))
        memset(existing, 0, sizeof(*existing));
    bool selected = code == MSM_GET_EVENT || code == MSM_ASYNC_READ ||
                    code == MSM_REGISTER || code == MSM_DEREGISTER ||
                    code == UINT32_C(0x40046100) || code == UINT32_C(0x40046101) ||
                    code == UINT32_C(0x40046102) || code == UINT32_C(0x4004610e);
    if (!selected) return -1;
    for (size_t index = 0; index < MSM_SESSIONS; ++index)
        if (msm_sessions[index].occupied && msm_sessions[index].fd != (int) tcp->u_arg[0])
            msm_multiple_fds = true;
    if (msm_multiple_fds) {
        tprints(", {msm_error=\"multiple_aac_fds_require_fresh_tracer\"}");
        return RVAL_IOCTL_DECODED;
    }
    if (entering(tcp)) {
        struct msm_session *session = msm_session((int) tcp->u_arg[0], true);
        struct msm_request *request = calloc(1, sizeof(*request));
        if (!session || !request) {
            free(request); tprints(", {msm_error=\"session_or_request_limit\"}");
            return RVAL_IOCTL_DECODED;
        }
        session->identity = identity;
        request->fd = session->fd; request->generation = session->generation;
        request->code = code; request->valid = true;
        if (code == MSM_REGISTER || code == MSM_DEREGISTER) {
            request->valid = !umoven(tcp, arg, 8, request->registration);
            if (request->valid && code == MSM_REGISTER)
                request->valid = msm_mapping_read(tcp, request->registration[1], &request->mapping, NULL);
        } else if (code == MSM_ASYNC_READ) {
            request->valid = !umoven(tcp, arg, sizeof(request->buffer), &request->buffer);
        } else if (code != MSM_GET_EVENT) {
            for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index)
                if (session->capture.pending[index].occupied) session->invalid = true;
            msm_capture_flush(&session->capture);
        }
        set_tcb_priv_data(tcp, request, free);
        tprintf(", {msm_request=%#x, generation=%" PRIu64, code, session->generation);
        if (code == MSM_ASYNC_READ) { tprints(", submitted="); msm_print_buffer(&request->buffer); }
        if (code == MSM_REGISTER || code == MSM_DEREGISTER)
            tprintf(", ion_fd=%u, registration_address=%#x, mapped_capacity=%u",
                    request->registration[0], request->registration[1], request->mapping.length);
        tprints(", entry_valid="); tprints(request->valid ? "true}" : "false}");
        return 0;
    }
    struct msm_request *request = get_tcb_priv_data(tcp);
    struct msm_session *session = msm_session((int) tcp->u_arg[0], false);
    const char *error = NULL;
    tprints(" => {");
    if (!request || !session || request->code != code || request->fd != session->fd ||
        request->generation != session->generation) error = "entry_or_generation_missing";
    else if (session->invalid) error = "session_invalidated";
    else if (syserror(tcp)) error = "ioctl_failed";
    else if (!request->valid) error = "entry_unreadable_or_mapping_missing";
    else if (code == MSM_REGISTER) {
        struct msm_mapping current;
        if (!msm_mapping_read(tcp, request->mapping.address, &current, NULL) ||
            strcmp(current.line, request->mapping.line)) error = "registration_mapping_changed";
        else {
            error = msm_capture_register(&session->capture, current.address, current.length);
            if (!error) {
                for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index)
                    if (!session->maps[index].length) { session->maps[index] = current; break; }
            }
        }
    } else if (code == MSM_DEREGISTER) {
        error = msm_capture_unregister(&session->capture, request->registration[1]);
        if (!error) for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index)
            if (session->maps[index].address == request->registration[1])
                memset(&session->maps[index], 0, sizeof(session->maps[index]));
    } else if (code == MSM_ASYNC_READ) error = msm_capture_submit(&session->capture, &request->buffer);
    else if (code == MSM_GET_EVENT) {
        tprints("event={");
        error = msm_finish_event(tcp, session, arg);
        tprints("}, ");
    }
    if (error && session && !syserror(tcp)) {
        session->invalid = true;
        msm_capture_flush(&session->capture);
    }
    if (error) tprintf("msm_error=\"%s\"", error);
    else tprints("msm_status=\"observed\"");
    tprints("}");
    return RVAL_IOCTL_DECODED;
}

#else
void msm_audio_invalidate_fd(struct tcb *tcp, int fd)
{ (void) tcp; (void) fd; }
int msm_audio_ioctl(struct tcb *tcp, unsigned int code, kernel_ulong_t arg)
{ (void) tcp; (void) code; (void) arg; return -1; }
#endif
