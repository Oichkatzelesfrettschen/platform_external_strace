#define _GNU_SOURCE
#include "msm_audio_glue_stubs.h"
#undef fopen
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "msm_audio_capture.h"
int msm_audio_ioctl(struct tcb *, unsigned int, kernel_ulong_t);
void msm_audio_invalidate_fd(struct tcb *, int);
static char output[300000];
static size_t used;
static const char *device_path = "/dev/msm_aac";
static unsigned char *mapping;
static bool fail_read, change_read;
static unsigned raw_reads;
static unsigned fail_raw_read, protect_raw_read;
static int raw_errno;
static int protected_permissions = PROT_READ;
static const char *smaps_fixture;
static int smaps_open_error;
static bool smaps_change_permissions;
unsigned current_wordsize = 4;
FILE *msm_test_fopen(const char *path, const char *mode)
{
    size_t length=strlen(path);
    if (length>=6 && !strcmp(path+length-6,"/smaps")) {
        if (smaps_open_error) { errno=smaps_open_error; return NULL; }
        if (smaps_fixture) {
            if (smaps_change_permissions) assert(mprotect(mapping,4096,PROT_READ)==0);
            return fmemopen((void *)smaps_fixture,strlen(smaps_fixture),"r");
        }
    }
    return fopen(path,mode);
}
static void make_smaps_fixture(char *destination, size_t length, const char *fields)
{
    char line[1024]; unsigned long long start, end;
    FILE *maps=fopen("/proc/self/maps","r"); assert(maps);
    while (fgets(line,sizeof(line),maps)) {
        assert(sscanf(line,"%llx-%llx",&start,&end)==2);
        if (start <= (uintptr_t)mapping+256 && (uintptr_t)mapping+256 < end) {
            int written=snprintf(destination,length,"%s%s",line,fields);
            assert(written>0 && (size_t)written<length);
            assert(fclose(maps)==0); return;
        }
    }
    assert(false);
}
void tprintf(const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = vsnprintf(output + used, sizeof(output) - used, format, args);
    va_end(args); assert(result >= 0 && (size_t)result < sizeof(output)-used); used += (size_t)result;
}
void tprints(const char *value) { tprintf("%s", value); }
int getfdpath(struct tcb *tcp, int fd, char *path, unsigned int length)
{ (void)tcp; (void)fd; return snprintf(path, length, "%s", device_path); }
int umoven(struct tcb *tcp, kernel_ulong_t address, unsigned int length, void *destination)
{
    (void)tcp;
    if (fail_read) return -1;
    if (address < (uintptr_t)mapping || address + length > (uintptr_t)mapping+4096) return -1;
    memcpy(destination, (void *)(uintptr_t)address, length);
    if (address == (uintptr_t)mapping + 256) {
        raw_reads++;
        if (raw_reads == fail_raw_read) { errno = raw_errno; return -1; }
        if (raw_reads == protect_raw_read) assert(mprotect(mapping,4096,protected_permissions)==0);
        if (change_read && (raw_reads % 2)==0) ((unsigned char *)destination)[60] ^= 1;
    }
    return 0;
}
void *get_tcb_priv_data(struct tcb *tcp) { return tcp->private_data; }
int set_tcb_priv_data(struct tcb *tcp, void *value, void (*release)(void *))
{ (void)release; free(tcp->private_data); tcp->private_data=value; return 0; }
static void run(struct tcb *tcp, unsigned int code, void *arg, bool failed)
{
    tcp->u_arg[0]=7; tcp->u_arg[1]=code; tcp->u_arg[2]=(uintptr_t)arg;
    tcp->entry=true; tcp->failed=false;
    assert(msm_audio_ioctl(tcp, code, tcp->u_arg[2]) == 0);
    tcp->entry=false; tcp->failed=failed;
    assert(msm_audio_ioctl(tcp, code, tcp->u_arg[2]) == RVAL_IOCTL_DECODED);
}
int main(void)
{
    char target[32]; snprintf(target, sizeof(target), "%d", getpid());
    assert(setenv("STRACE_MSM_AUDIO_CAPTURE_PID",target,1)==0);
    mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(mapping != MAP_FAILED && (uintptr_t)mapping <= UINT32_MAX-4096);
    struct tcb tcp={.pid=getpid(),.entry=true,.u_arg={7,0,0}};
    int device_fd = open("/dev/null", O_RDONLY);
    assert(device_fd >= 0 && dup2(device_fd, 7) == 7);
    if (device_fd != 7) assert(close(device_fd) == 0);
    current_wordsize = 8;
    assert(msm_audio_ioctl(&tcp,0x8004610d,0)==-1);
    current_wordsize = 4;
    device_path="/dev/unrelated";
    assert(msm_audio_ioctl(&tcp,0x8004610d,0)==-1);
    device_path="/dev/msm_aac";
    assert(msm_audio_ioctl(&tcp,0x8008610d,0)==-1);
    uint32_t *registration=(uint32_t *)mapping;
    registration[0]=8; registration[1]=(uint32_t)(uintptr_t)(mapping+256);
    run(&tcp,0x40046161,registration,false);
    assert(strstr(output,"mapped_capacity=3840"));
    struct msm_capture_aio *buffer=(void *)(mapping+32);
    *buffer=(struct msm_capture_aio){registration[1],128,80,1234,32,0};
    run(&tcp,0x40046112,buffer,true);
    assert(strstr(output,"ioctl_failed"));
    uint32_t *event=(uint32_t *)(mapping+64); event[0]=3;
    memcpy(event+2,buffer,sizeof(*buffer));
    run(&tcp,0x8004610d,event,false);
    assert(strstr(output,"completion_unmatched") && raw_reads==0);
    msm_audio_invalidate_fd(&tcp,7);
    run(&tcp,0x40046161,registration,false);
    run(&tcp,0x40046112,buffer,false);
    uint32_t *header=(uint32_t *)(mapping+256);
    header[7]=1; header[8]=24; header[9]=24; header[10]=2;
    run(&tcp,0x8004610d,event,false);
    assert(raw_reads==2 && strstr(output,"samples=2") && strstr(output,"raw_hex="));
    run(&tcp,0x40046112,buffer,false);
    change_read=true;
    run(&tcp,0x8004610d,event,false);
    assert(strstr(output,"capture_changed"));
    change_read=false;
    used=0; output[0]=0;
    run(&tcp,0x40046112,buffer,false);
    assert(strstr(output,"session_invalidated"));
    msm_audio_invalidate_fd(&tcp,7);
    run(&tcp,0x8004610d,event,false);
    assert(strstr(output,"completion_mapping_missing"));
    msm_audio_invalidate_fd(&tcp,7);
    run(&tcp,0x40046161,registration,false);
    run(&tcp,0x40046112,buffer,false);
    assert(mprotect(mapping,4096,PROT_READ)==0);
    run(&tcp,0x8004610d,event,false);
    assert(strstr(output,"capture_unreadable_or_remapped"));
    assert(strstr(output,"stage=\"mapping_before_identity\""));
    assert(strstr(output,"snapshot=1") && strstr(output,"errno=0"));
    assert(mprotect(mapping,4096,PROT_READ|PROT_WRITE)==0);
    const int expected_errors[] = {EFAULT, EIO, 0};
    for (size_t error_index=0; error_index<3; ++error_index) {
        for (unsigned snapshot=1; snapshot<=2; ++snapshot) {
            msm_audio_invalidate_fd(&tcp,7);
            run(&tcp,0x40046161,registration,false);
            run(&tcp,0x40046112,buffer,false);
            used=0; output[0]=0; raw_reads=0;
            fail_raw_read=snapshot; raw_errno=expected_errors[error_index];
            run(&tcp,0x8004610d,event,false);
            char expected[128];
            snprintf(expected,sizeof(expected),"snapshot=%u, stage=\"umoven\", read_result=-1, errno=%d",
                     snapshot,raw_errno);
            assert(strstr(output,expected));
            assert(strstr(output,"capture_unreadable_or_remapped"));
            assert(!strstr(output,"raw_hex="));
            used=0; output[0]=0;
            run(&tcp,0x40046112,buffer,false);
            assert(strstr(output,"session_invalidated"));
        }
    }
    fail_raw_read=0;
    for (unsigned snapshot=1; snapshot<=2; ++snapshot) {
        msm_audio_invalidate_fd(&tcp,7);
        run(&tcp,0x40046161,registration,false);
        run(&tcp,0x40046112,buffer,false);
        used=0; output[0]=0; raw_reads=0; protect_raw_read=snapshot;
        run(&tcp,0x8004610d,event,false);
        char expected[128];
        snprintf(expected,sizeof(expected),"snapshot=%u, stage=\"mapping_after_identity\", read_result=0, errno=0",snapshot);
        assert(strstr(output,expected));
        assert(!strstr(output,"raw_hex="));
        assert(mprotect(mapping,4096,PROT_READ|PROT_WRITE)==0);
    }
    protected_permissions=PROT_NONE;
    for (unsigned snapshot=1; snapshot<=2; ++snapshot) {
        msm_audio_invalidate_fd(&tcp,7);
        run(&tcp,0x40046161,registration,false);
        run(&tcp,0x40046112,buffer,false);
        used=0; output[0]=0; raw_reads=0; protect_raw_read=snapshot;
        run(&tcp,0x8004610d,event,false);
        char expected[128];
        snprintf(expected,sizeof(expected),"snapshot=%u, stage=\"mapping_after_lookup\", read_result=0, errno=0",snapshot);
        assert(strstr(output,expected));
        assert(strstr(output,"mapping_reason=\"mapping_not_readable\""));
        assert(!strstr(output,"raw_hex="));
        assert(mprotect(mapping,4096,PROT_READ|PROT_WRITE)==0);
    }
    protect_raw_read=0;
    char synthetic_smaps[4096];
    const char *flag_fields[]={"VmFlags: rd wr sh mr mw me ms io pf \n", "VmFlags: rd wr mr mw me ac \n", "Rss: 4 kB\n", "VmFlags: rd \"pf\n"};
    const char *expected_flags[]={"io=true, pf=true", "io=false, pf=false", "reason=\"vmflags_missing\"", "reason=\"vmflags_invalid\""};
    for (size_t flag_case=0; flag_case<4; ++flag_case) {
        msm_audio_invalidate_fd(&tcp,7);
        run(&tcp,0x40046161,registration,false);
        run(&tcp,0x40046112,buffer,false);
        make_smaps_fixture(synthetic_smaps,sizeof(synthetic_smaps),flag_fields[flag_case]);
        smaps_fixture=synthetic_smaps;
        used=0; output[0]=0; raw_reads=0; fail_raw_read=1; raw_errno=EFAULT;
        run(&tcp,0x8004610d,event,false);
        assert(strstr(output,expected_flags[flag_case]));
        assert(strstr(output,"capture_unreadable_or_remapped") && !strstr(output,"raw_hex="));
    }
    for (unsigned boundary_case=0; boundary_case<4; ++boundary_case) {
        msm_audio_invalidate_fd(&tcp,7);
        run(&tcp,0x40046161,registration,false);
        run(&tcp,0x40046112,buffer,false);
        make_smaps_fixture(synthetic_smaps,sizeof(synthetic_smaps),flag_fields[0]);
        const char *expected_reason;
        char *large_fixture=NULL;
        if (boundary_case==0) {
            char *permissions=strstr(synthetic_smaps," rw"); assert(permissions); permissions[2]='-';
            expected_reason="reason=\"mapping_identity\"";
        } else if (boundary_case==1) {
            smaps_change_permissions=true;
            expected_reason="reason=\"maps_after_flags_identity\"";
        } else if (boundary_case==2) {
            memset(synthetic_smaps,'x',sizeof(synthetic_smaps)-1); synthetic_smaps[sizeof(synthetic_smaps)-1]=0;
            expected_reason="reason=\"smaps_line_overflow\"";
        } else {
            size_t length=4U*1024U*1024U;
            large_fixture=malloc(length+1); assert(large_fixture);
            memset(large_fixture,' ',length);
            for (size_t index=127; index<length; index+=128) large_fixture[index]='\n';
            large_fixture[length]=0;
            expected_reason="reason=\"smaps_scan_limit\"";
        }
        smaps_fixture=large_fixture ? large_fixture : synthetic_smaps;
        used=0; output[0]=0; raw_reads=0;
        run(&tcp,0x8004610d,event,false);
        assert(strstr(output,expected_reason));
        assert(strstr(output,"capture_unreadable_or_remapped") && !strstr(output,"raw_hex="));
        smaps_change_permissions=false;
        assert(mprotect(mapping,4096,PROT_READ|PROT_WRITE)==0);
        free(large_fixture);
    }
    smaps_fixture=NULL; smaps_open_error=EACCES;
    msm_audio_invalidate_fd(&tcp,7);
    run(&tcp,0x40046161,registration,false);
    run(&tcp,0x40046112,buffer,false);
    used=0; output[0]=0; raw_reads=0;
    run(&tcp,0x8004610d,event,false);
    assert(strstr(output,"status=\"unavailable\", reason=\"smaps_open\", errno=13"));
    smaps_open_error=0; fail_raw_read=0;
    msm_audio_invalidate_fd(&tcp,7);
    run(&tcp,0x40046161,registration,false);
    fail_read=true;
    run(&tcp,0x40046112,buffer,false);
    fail_read=false;
    assert(strstr(output,"entry_unreadable_or_mapping_missing"));
    tcp.entry=false;
    free(tcp.private_data); tcp.private_data=NULL;
    assert(msm_audio_ioctl(&tcp,0x8004610d,(uintptr_t)event)==RVAL_IOCTL_DECODED);
    assert(strstr(output,"entry_or_generation_missing"));
    tcp.entry=true;
    assert(dup2(7,8)==8);
    tcp.u_arg[0]=8;
    assert(msm_audio_ioctl(&tcp,0x8004610d,0)==RVAL_IOCTL_DECODED);
    assert(strstr(output,"multiple_aac_fds_require_fresh_tracer"));
    tcp.u_arg[0]=7;
    assert(close(8)==0);
    assert(close(7)==0);
    assert(msm_audio_ioctl(&tcp,0x8004610d,0)==-1);
    free(tcp.private_data); assert(munmap(mapping,4096)==0);
    puts("msm audio real-glue fixtures pass");
    return 0;
}
