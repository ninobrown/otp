/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2006-2010. All Rights Reserved.
 *
 * The contents of this file are subject to the Erlang Public License,
 * Version 1.1, (the "License"); you may not use this file except in
 * compliance with the License. You should have received a copy of the
 * Erlang Public License along with this software. If not, it can be
 * retrieved online at http://www.erlang.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * %CopyrightEnd%
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "erl_misc_utils.h"

#if defined(__WIN32__)
#  include <windows.h>
#elif defined(VXWORKS)
#  include <selectLib.h>
#else /* UNIX */
#  include <stdio.h>
#  include <sys/types.h>
#  include <sys/param.h>
#  include <limits.h>
#  include <dirent.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  ifdef SYS_SELECT_H
#    include <sys/select.h>
#  endif
#  if TIME_WITH_SYS_TIME
#     include <sys/time.h>
#     include <time.h>
#  else
#     if HAVE_SYS_TIME_H
#         include <sys/time.h>
#     else
#         include <time.h>
#     endif
#  endif
#  include <string.h>
#  ifdef HAVE_UNISTD_H
#    include <unistd.h>
#  endif
#  if (defined(NO_SYSCONF) || !defined(_SC_NPROCESSORS_CONF))
#    ifdef HAVE_SYS_SYSCTL_H
#      include <sys/sysctl.h>
#    endif
#  endif
#endif

#if defined(HAVE_SCHED_xETAFFINITY)
#  include <sched.h>
#  define ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__
#define ERTS_MU_GET_PROC_AFFINITY__(CPUINFOP)				\
     (sched_getaffinity((CPUINFOP)->pid,				\
			sizeof(cpu_set_t),				\
			&(CPUINFOP)->cpuset) != 0 ? -errno : 0)
#define ERTS_MU_SET_THR_AFFINITY__(SETP)				\
     (sched_setaffinity(0, sizeof(cpu_set_t), (SETP)) != 0 ? -errno : 0)
#elif defined(__WIN32__)
#  define ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__
#  define cpu_set_t DWORD
#  define CPU_SETSIZE (sizeof(DWORD)*8)
#  define CPU_ZERO(SETP) (*(SETP) = (DWORD) 0)
#  define CPU_SET(CPU, SETP) (*(SETP) |= (((DWORD) 1) << (CPU)))
#  define CPU_CLR(CPU, SETP) (*(SETP) &= ~(((DWORD) 1) << (CPU)))
#  define CPU_ISSET(CPU, SETP) ((*(SETP) & (((DWORD) 1) << (CPU))) != (DWORD) 0)
#define ERTS_MU_GET_PROC_AFFINITY__ get_proc_affinity
#define ERTS_MU_SET_THR_AFFINITY__ set_thr_affinity
#endif
#ifdef HAVE_PSET_INFO
#  include <sys/pset.h>
#endif
#ifdef HAVE_PROCESSOR_BIND
#  include <sys/processor.h>
#  include <sys/procset.h>
#endif

#include <stdlib.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif

#ifdef __linux__
#  define ERTS_SYS_NODE_PATH	"/sys/devices/system/node"
#  define ERTS_SYS_CPU_PATH	"/sys/devices/system/cpu"
#endif

static int read_topology(erts_cpu_info_t *cpuinfo);

int
erts_milli_sleep(long ms)
{
    if (ms > 0) {
#ifdef __WIN32__
	Sleep((DWORD) ms);
#else
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	if (select(0, NULL, NULL, NULL, &tv) < 0)
	    return errno == EINTR ? 1 : -1;
#endif
    }
    return 0;
}

struct erts_cpu_info_t_ {
    int configured;
    int online;
    int available;
    int topology_size;
    erts_cpu_topology_t *topology;
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    char *affinity_str;
    char affinity_str_buf[CPU_SETSIZE/4+2];
    cpu_set_t cpuset;
#if defined(HAVE_SCHED_xETAFFINITY)
    pid_t pid;
#endif
#elif defined(HAVE_PSET_INFO)
    processorid_t *cpuids;
#endif
};

#if defined(__WIN32__)

static __forceinline int
get_proc_affinity(erts_cpu_info_t *cpuinfo)
{
    DWORD pamask, samask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &pamask, &samask)) {
	cpuinfo->cpuset = (cpu_set_t) pamask;
	return 0;
    }
    else {
	cpuinfo->cpuset = (cpu_set_t) 0;
	return -erts_get_last_win_errno();
    }
}

static __forceinline int
set_thr_affinity(cpu_set_t *set)
{
    if (*set == (cpu_set_t) 0)
	return -ENOTSUP;
    if (SetThreadAffinityMask(GetCurrentThread(), *set) == 0)
	return -erts_get_last_win_errno();
    else
	return 0;
}

#endif

erts_cpu_info_t *
erts_cpu_info_create(void)
{
    erts_cpu_info_t *cpuinfo = malloc(sizeof(erts_cpu_info_t));
    if (!cpuinfo)
	return NULL;
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    cpuinfo->affinity_str = NULL;
#if defined(HAVE_SCHED_xETAFFINITY)
    cpuinfo->pid = getpid();
#endif
#elif defined(HAVE_PSET_INFO)
    cpuinfo->cpuids = NULL;
#endif
    cpuinfo->topology_size = 0;
    cpuinfo->topology = NULL;
    erts_cpu_info_update(cpuinfo);
    return cpuinfo;
}

void
erts_cpu_info_destroy(erts_cpu_info_t *cpuinfo)
{
    if (cpuinfo) {
	cpuinfo->configured = 0;
	cpuinfo->online = 0;
	cpuinfo->available = 0;
#ifdef HAVE_PSET_INFO
	if (cpuinfo->cpuids)
	    free(cpuinfo->cpuids);
#endif
	cpuinfo->topology_size = 0;
	if (cpuinfo->topology) {
	    cpuinfo->topology = NULL;
	    free(cpuinfo->topology);
	}
	free(cpuinfo);
    }
}

void
erts_cpu_info_update(erts_cpu_info_t *cpuinfo)
{
    cpuinfo->configured = 0;
    cpuinfo->online = 0;
    cpuinfo->available = 0;

#ifdef __WIN32__
    {
	int i;
	SYSTEM_INFO sys_info;
	GetSystemInfo(&sys_info);
	cpuinfo->configured = (int) sys_info.dwNumberOfProcessors;
	for (i = 0; i < sizeof(DWORD)*8; i++)
	    if (sys_info.dwActiveProcessorMask & (((DWORD) 1) << i))
		cpuinfo->online++;
    }
#elif !defined(NO_SYSCONF) && (defined(_SC_NPROCESSORS_CONF) \
			       || defined(_SC_NPROCESSORS_ONLN))
#ifdef _SC_NPROCESSORS_CONF
    cpuinfo->configured = (int) sysconf(_SC_NPROCESSORS_CONF);
    if (cpuinfo->configured < 0)
	cpuinfo->configured = 0;
#endif
#ifdef _SC_NPROCESSORS_ONLN
    cpuinfo->online = (int) sysconf(_SC_NPROCESSORS_ONLN);
    if (cpuinfo->online < 0)
	cpuinfo->online = 0;
#endif
#elif defined(HAVE_SYS_SYSCTL_H) && defined(CTL_HW) && (defined(HW_NCPU) \
							|| defined(HW_AVAILCPU))
    {
	int mib[2];
	size_t len;

#ifdef HW_NCPU
	len = sizeof(int);
	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;
	if (sysctl(&mib[0], 2, &cpuinfo->configured, &len, NULL, 0) < 0)
	    cpuinfo->configured = 0;
#endif
#ifdef HW_AVAILCPU
	len = sizeof(int);
	mib[0] = CTL_HW;
	mib[1] = HW_AVAILCPU;
	if (sysctl(&mib[0], 2, &cpuinfo->online, &len, NULL, 0) < 0)
	    cpuinfo->online = 0;
#endif
    }
#endif

    if (cpuinfo->online > cpuinfo->configured)
	cpuinfo->online = cpuinfo->configured;

#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    if (ERTS_MU_GET_PROC_AFFINITY__(cpuinfo) == 0) {
	int i, c, cn, si;
	c = cn = 0;
	si = sizeof(cpuinfo->affinity_str_buf) - 1;
	cpuinfo->affinity_str_buf[si] = '\0';
	for (i = 0; i < CPU_SETSIZE; i++) {
	    if (CPU_ISSET(i, &cpuinfo->cpuset)) {
		c |= 1 << cn;
		cpuinfo->available++;
	    }
	    cn++;
	    if (cn == 4) {
		cpuinfo->affinity_str_buf[--si] = (c < 10
						   ? '0' + c
						   : 'A' + c - 10);
		c = cn = 0;
	    }
	}
	if (c)
	    cpuinfo->affinity_str_buf[--si] = (c < 10
					       ? '0' + c
					       : 'A' + c - 10);
	while (cpuinfo->affinity_str_buf[si] == '0')
	    si++;
	cpuinfo->affinity_str = &cpuinfo->affinity_str_buf[si];
    }
#elif defined(HAVE_PSET_INFO)
    {
	uint_t numcpus = cpuinfo->configured;
	if (cpuinfo->cpuids)
	    free(cpuinfo->cpuids);
	cpuinfo->cpuids = malloc(sizeof(processorid_t)*numcpus);
	if (cpuinfo->cpuids) {
	    if (pset_info(PS_MYID, NULL, &numcpus, &cpuinfo->cpuids) == 0)
		cpuinfo->available = (int) numcpus;
	    if (cpuinfo->available < 0) {
		free(cpuinfo->cpuid);
		cpuinfo->available = 0;
	    }
	}
    }
#endif

    if (cpuinfo->available > cpuinfo->online)
	cpuinfo->available = cpuinfo->online;

    read_topology(cpuinfo);

}

int
erts_get_cpu_configured(erts_cpu_info_t *cpuinfo)
{
    if (!cpuinfo)
	return -EINVAL;
    if (cpuinfo->configured <= 0)
	return -ENOTSUP;
    return cpuinfo->configured;
}

int
erts_get_cpu_online(erts_cpu_info_t *cpuinfo)
{
    if (!cpuinfo)
	return -EINVAL;
    if (cpuinfo->online <= 0)
	return -ENOTSUP;
    return cpuinfo->online;
}

int
erts_get_cpu_available(erts_cpu_info_t *cpuinfo)
{
    if (!cpuinfo)
	return -EINVAL;
    if (cpuinfo->available <= 0)
	return -ENOTSUP;
    return cpuinfo->available;
}

char *
erts_get_unbind_from_cpu_str(erts_cpu_info_t *cpuinfo)
{
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    if (!cpuinfo)
	return "false";
    return cpuinfo->affinity_str;
#else
    return "true";
#endif
}

int
erts_get_available_cpu(erts_cpu_info_t *cpuinfo, int no)
{
    if (!cpuinfo || no < 1 || cpuinfo->available < no)
	return -EINVAL;
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    {
	cpu_set_t *allowed = &cpuinfo->cpuset;
	int ix, n;
	for (ix = 0, n = 1; ix < CPU_SETSIZE; ix++) {
	    if (CPU_ISSET(ix, allowed)) {
		if (no == n)
		    return ix;
		n++;
	    }
	}
    }
    return -EINVAL;
#elif defined(HAVE_PROCESSOR_BIND)
#if defined(HAVE_PSET_INFO)
    return (int) cpuinfo->cpuids[no-1];
#elif defined(HAVE_KSTAT)
    if (cpuinfo->topology && cpuinfo->online <= no) {
	/* May not be available, but this is the best we can do */
	return cpuinfo->topology[no-1].logical;
    }
    return -EINVAL;
#endif
#else
    return -ENOTSUP;
#endif
}

int
erts_is_cpu_available(erts_cpu_info_t *cpuinfo, int id)
{
    if (cpuinfo && 0 <= id) {
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
	if (id < CPU_SETSIZE)
	    return CPU_ISSET(id, &cpuinfo->cpuset);
#elif defined(HAVE_PROCESSOR_BIND)
	int no;
#if defined(HAVE_PSET_INFO)
	for (no = 0; no < cpuinfo->available; no++)
	    if (id == (int) cpuinfo->cpuids[no])
		return 1;
#elif defined(HAVE_KSTAT)
	if (cpuinfo->topology) {
	    for (no = 0; no < cpuinfo->online; no++) {
		if (id == (int) cpuinfo->topology[no].logical) {
		    /* May not be available, but this is the best we can do... */
		    return 1;
		}
	    }
	}
#endif
#endif
    }
    return 0;
}

int
erts_get_cpu_topology_size(erts_cpu_info_t *cpuinfo)
{
    return cpuinfo->topology_size;
}

int
erts_get_cpu_topology(erts_cpu_info_t *cpuinfo,
		      erts_cpu_topology_t *topology)
{
    if (!cpuinfo->topology)
	return 0;
    memcpy((void *) topology,
	   (void *) cpuinfo->topology,
	   cpuinfo->topology_size*sizeof(erts_cpu_topology_t));
    return cpuinfo->topology_size;
}

int
erts_bind_to_cpu(erts_cpu_info_t *cpuinfo, int cpu)
{
    /*
     * Caller can test for available functionality by
     * passing a negative cpu id. If functionality is
     * available -EINVAL is returned; otherwise,
     * -ENOTSUP.
     */
    if (!cpuinfo)
	return -EINVAL;
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    {
	cpu_set_t bind_set;
	if (cpu < 0)
	    return -EINVAL;
	if (!CPU_ISSET(cpu, &cpuinfo->cpuset))
	    return -EINVAL;

	CPU_ZERO(&bind_set);
	CPU_SET(cpu, &bind_set);
	return ERTS_MU_SET_THR_AFFINITY__(&bind_set);
    }
#elif defined(HAVE_PROCESSOR_BIND)
    if (cpu < 0)
	return -EINVAL;
    if (processor_bind(P_LWPID, P_MYID, (processorid_t) cpu, NULL) != 0)
	return -errno;
    return 0;
#else
    return -ENOTSUP;
#endif
}

int
erts_unbind_from_cpu(erts_cpu_info_t *cpuinfo)
{
    if (!cpuinfo)
	return -EINVAL;
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    return ERTS_MU_SET_THR_AFFINITY__(&cpuinfo->cpuset);
#elif defined(HAVE_PROCESSOR_BIND)
    if (processor_bind(P_LWPID, P_MYID, PBIND_NONE, NULL) != 0)
	return -errno;
    return 0;
#else
    return -ENOTSUP;
#endif
}

int
erts_unbind_from_cpu_str(char *str)
{
#if defined(ERTS_HAVE_MISC_UTIL_AFFINITY_MASK__)
    char *c = str;
    int cpus = 0;
    int shft = 0;
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);

    if (!c)
	return -EINVAL;

    while (*c)
	c++;

    while (c != str) {
	int shft2;
	int mask = 0;
	c--;
	switch (*c) {
	case '0': mask = 0; break;
	case '1': mask = 1; break;
	case '2': mask = 2; break;
	case '3': mask = 3; break;
	case '4': mask = 4; break;
	case '5': mask = 5; break;
	case '6': mask = 6; break;
	case '7': mask = 7; break;
	case '8': mask = 8; break;
	case '9': mask = 9; break;
	case 'A': case 'a': mask = 10; break;
	case 'B': case 'b': mask = 11; break;
	case 'C': case 'c': mask = 12; break;
	case 'D': case 'd': mask = 13; break;
	case 'E': case 'e': mask = 14; break;
	case 'F': case 'f': mask = 15; break;
	default: return -EINVAL;
	}
	for (shft2 = 0; shft2 < 4; shft2++) {
	    if (mask & (1 << shft2)) {
		int cpu = shft + shft2;
		if (cpu >= CPU_SETSIZE)
		    return -EINVAL;
		cpus++;
		CPU_SET(cpu, &cpuset);
	    }
	}
	shft += 4;
    }

    if (!cpus)
	return -EINVAL;

    return ERTS_MU_SET_THR_AFFINITY__(&cpuset);
#elif defined(HAVE_PROCESSOR_BIND)
    if (processor_bind(P_LWPID, P_MYID, PBIND_NONE, NULL) != 0)
	return -errno;
    return 0;
#else
    return -ENOTSUP;
#endif
}


static int
pn_cmp(const void *vx, const void *vy)
{
    erts_cpu_topology_t *x = (erts_cpu_topology_t *) vx;
    erts_cpu_topology_t *y = (erts_cpu_topology_t *) vy;

    if (x->processor != y->processor)
	return x->processor - y->processor;
    if (x->node != y->node)
	return x->node - y->node;
    if (x->processor_node != y->processor_node)
	return x->processor_node - y->processor_node;
    if (x->core != y->core)
	return x->core - y->core;
    if (x->thread != y->thread)
	return x->thread - y->thread;
    if (x->logical != y->logical)
	return x->logical - y->logical;
    return 0;
}

static int
cpu_cmp(const void *vx, const void *vy)
{
    erts_cpu_topology_t *x = (erts_cpu_topology_t *) vx;
    erts_cpu_topology_t *y = (erts_cpu_topology_t *) vy;

    if (x->node != y->node)
	return x->node - y->node;
    if (x->processor != y->processor)
	return x->processor - y->processor;
    if (x->processor_node != y->processor_node)
	return x->processor_node - y->processor_node;
    if (x->core != y->core)
	return x->core - y->core;
    if (x->thread != y->thread)
	return x->thread - y->thread;
    if (x->logical != y->logical)
	return x->logical - y->logical;
    return 0;
}

#ifdef __linux__

static int
read_file(char *path, char *buf, int size)
{
    int ix = 0;
    ssize_t sz = size-1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
	goto error;
    while (size > ix) {
	sz = read(fd, &buf[ix], size - ix);
	if (sz <= 0) {
	    if (sz == 0)
		break;
	    if (errno == EINTR)
		continue;
	    goto error;
	}
	ix += sz;
    }
    buf[ix] = '\0';
    close(fd);
    return ix;

 error: {
	int saved_errno = errno;
	if (fd >= 0)
	    close(fd);
	if (saved_errno)
	    return -saved_errno;
	else
	    return -EINVAL;
    }
}

static int
read_topology(erts_cpu_info_t *cpuinfo)
{
    char npath[MAXPATHLEN];
    char cpath[MAXPATHLEN];
    char tpath[MAXPATHLEN];
    char fpath[MAXPATHLEN];
    DIR *ndir = NULL;
    DIR *cdir = NULL;
    struct dirent *nde;
    int ix;
    int res = 0;
    int got_nodes = 0;
    int no_nodes = 0;

    errno = 0;

    if (cpuinfo->topology)
	free(cpuinfo->topology);

    if (cpuinfo->configured < 1)
	goto error;

    cpuinfo->topology = malloc(sizeof(erts_cpu_topology_t)
			       * cpuinfo->configured);
    if (!cpuinfo)
	goto error;

    for (ix = 0; ix < cpuinfo->configured; ix++) {
	cpuinfo->topology[ix].node = -1;
	cpuinfo->topology[ix].processor = -1;
	cpuinfo->topology[ix].processor_node = -1;
	cpuinfo->topology[ix].core = -1;
	cpuinfo->topology[ix].thread = -1;
	cpuinfo->topology[ix].logical = -1;
    }

    ix = -1;

    if (realpath(ERTS_SYS_NODE_PATH, npath)) {
	got_nodes = 1;
	ndir = opendir(npath);
    }

    do {
	int node_id = -1;

	if (!got_nodes) {
	    if (!realpath(ERTS_SYS_CPU_PATH, cpath))
		goto error;
	}
	else {

	    nde = readdir(ndir);

	    if (!nde)
		break;

	    if (sscanf(nde->d_name, "node%d", &node_id) != 1)
		continue;

	    no_nodes++;

	    sprintf(tpath, "%s/node%d", npath, node_id);

	    if (!realpath(tpath, cpath))
		goto error;
	}

	cdir = opendir(cpath);
	if (!cdir)
	    goto error;

	while (1) {
	    int cpu_id;
	    struct dirent *cde = readdir(cdir);
	    if (!cde) {
		closedir(cdir);
		cdir = NULL;
		break;
	    }
	    if (sscanf(cde->d_name, "cpu%d", &cpu_id) == 1) {
		char buf[50]; /* Much more than enough for an integer */
		int processor_id, core_id;
		sprintf(tpath, "%s/cpu%d/topology/physical_package_id",
			cpath, cpu_id);
		if (!realpath(tpath, fpath))
		    continue;
		if (read_file(fpath, buf, sizeof(buf)) <= 0)
		    continue;
		if (sscanf(buf, "%d", &processor_id) != 1)
		    continue;
		sprintf(tpath, "%s/cpu%d/topology/core_id",
			cpath, cpu_id);
		if (!realpath(tpath, fpath))
		    continue;
		if (read_file(fpath, buf, sizeof(buf)) <= 0)
		    continue;
		if (sscanf(buf, "%d", &core_id) != 1)
		    continue;

		/*
		 * We now know node id, processor id, and
		 * core id of the logical processor with
		 * the cpu id 'cpu_id'.
		 */
		ix++;
		cpuinfo->topology[ix].node	= node_id;
		cpuinfo->topology[ix].processor	= processor_id;
		cpuinfo->topology[ix].processor_node = -1; /* Fixed later */
		cpuinfo->topology[ix].core	= core_id;
		cpuinfo->topology[ix].thread	= 0; /* we'll numerate later */
		cpuinfo->topology[ix].logical	= cpu_id;
	    }
	}
    } while (got_nodes);

    res = ix+1;

    if (!res || res < cpuinfo->online)
	res = 0;
    else {
	erts_cpu_topology_t *prev, *this, *last;

	cpuinfo->topology_size = res;

	if (cpuinfo->topology_size != cpuinfo->configured) {
	    void *t = realloc(cpuinfo->topology, (sizeof(erts_cpu_topology_t)
						  * cpuinfo->topology_size));
	    if (t)
		cpuinfo->topology = t;
	}

	if (no_nodes > 1) {
	    int processor = -1;
	    int processor_node = 0;
	    int node = -1;

	    qsort(cpuinfo->topology,
		  cpuinfo->topology_size,
		  sizeof(erts_cpu_topology_t),
		  pn_cmp);

	    prev = NULL;
	    this = &cpuinfo->topology[0];
	    last = &cpuinfo->topology[cpuinfo->configured-1];
	    while (1) {
		if (processor == this->processor) {
		    if (node != this->node)
			processor_node = 1;
		}
		else {
		    if (processor_node) {
		    make_processor_node:
			while (prev->processor == processor) {
			    prev->processor_node = prev->node;
			    prev->node = -1;
			    if (prev == &cpuinfo->topology[0])
				break;
			    prev--;
			}
			processor_node = 0;
		    }
		    processor = this->processor;
		    node = this->node;
		}
		if (this == last) {
		    if (processor_node) {
			prev = this;
			goto make_processor_node;
		    }
		    break;
		}
		prev = this++;
	    }
	}

	qsort(cpuinfo->topology,
	      cpuinfo->topology_size,
	      sizeof(erts_cpu_topology_t),
	      cpu_cmp);

	this = &cpuinfo->topology[0];
	this->thread = 0;

	if (res > 1) {
	    prev = this++;
	    last = &cpuinfo->topology[cpuinfo->configured-1];

	    while (1) {
		this->thread = ((this->node == prev->node
				 && this->processor == prev->processor
				 && this->processor_node == prev->processor_node
				 && this->core == prev->core)
				? prev->thread + 1
				: 0);
		if (this == last)
		    break;
		prev = this++;
	    }
	}
    }

 error:

    if (res == 0) {
	cpuinfo->topology_size = 0;
	if (cpuinfo->topology) {
	    free(cpuinfo->topology);
	    cpuinfo->topology = NULL;
	}
	if (errno)
	    res = -errno;
	else
	    res = -EINVAL;
    }

    if (ndir)
	closedir(ndir);
    if (cdir)
	closedir(cdir);

    return res;
}

#elif defined(HAVE_KSTAT) /* SunOS kstat */

#include <kstat.h>

static int
data_lookup_int(kstat_t *ks, char *what)
{
    int res;
    kstat_named_t *ks_n;

    ks_n = kstat_data_lookup(ks, what);
    if (!ks_n)
	return 0;

    switch (ks_n->data_type) {
    case KSTAT_DATA_CHAR:
	res = atoi(ks_n->value.c);
	break;
    case KSTAT_DATA_INT32:
	res = (int) ks_n->value.i32;
	break;
    case KSTAT_DATA_UINT32:
	res = (int) ks_n->value.ui32;
	break;
    case KSTAT_DATA_INT64:
	res = (int) ks_n->value.i64;
	break;
    case KSTAT_DATA_UINT64:
	res = (int) ks_n->value.ui64;
	break;
    default:
	res = 0;
	break;
    }
    return res;
}

static int
read_topology(erts_cpu_info_t *cpuinfo)
{
    int res = 0;
    int ix;
    kstat_ctl_t *ks_ctl;
    kstat_t *ks;

    errno = 0;

    if (cpuinfo->topology)
	free(cpuinfo->topology);

    if (cpuinfo->configured < 1)
	goto error;

    cpuinfo->topology = malloc(sizeof(erts_cpu_topology_t)
			       * cpuinfo->configured);
    if (!cpuinfo)
	goto error;

    for (ix = 0; ix < cpuinfo->configured; ix++) {
	cpuinfo->topology[ix].node = -1;
	cpuinfo->topology[ix].processor = -1;
	cpuinfo->topology[ix].processor_node = -1;
	cpuinfo->topology[ix].core = -1;
	cpuinfo->topology[ix].thread = -1;
	cpuinfo->topology[ix].logical = -1;
    }

    ks_ctl = kstat_open();
    if (!ks_ctl)
	goto error;

    ix = 0;
    for (ks = ks_ctl->kc_chain; ks; ks = ks->ks_next) {
	if (strcmp("cpu_info", ks->ks_module) == 0) {
	    kstat_read(ks_ctl, ks, NULL);
	    if (ks->ks_type == KSTAT_TYPE_NAMED) {
		/*
		 * Don't know how to figure numa nodes out;
		 * hope there is none...
		 */
		cpuinfo->topology[ix].node = -1;
		cpuinfo->topology[ix].processor = data_lookup_int(ks,"chip_id");
		cpuinfo->topology[ix].processor_node = -1;
		cpuinfo->topology[ix].core = data_lookup_int(ks, "core_id");
		cpuinfo->topology[ix].thread = 0; /* we'll numerate later */
		cpuinfo->topology[ix].logical = ks->ks_instance;
		if (++ix == cpuinfo->configured)
		    break;
	    }
	}
    }

    kstat_close(ks_ctl);

    res = ix;

    if (!res || res < cpuinfo->online)
	res = 0;
    else {
	erts_cpu_topology_t *prev, *this, *last;

	cpuinfo->topology_size = res;

	if (cpuinfo->topology_size != cpuinfo->configured) {
	    void *t = realloc(cpuinfo->topology, (sizeof(erts_cpu_topology_t)
						  * cpuinfo->topology_size));
	    if (t)
		cpuinfo->topology = t;
	}

	qsort(cpuinfo->topology,
	      cpuinfo->topology_size,
	      sizeof(erts_cpu_topology_t),
	      cpu_cmp);

	this = &cpuinfo->topology[0];
	this->thread = 0;

	if (res > 1) {
	    prev = this++;
	    last = &cpuinfo->topology[cpuinfo->configured-1];

	    while (1) {
		this->thread = ((this->node == prev->node
				 && this->processor == prev->processor
				 && this->processor_node == prev->processor_node
				 && this->core == prev->core)
				? prev->thread + 1
				: 0);
		if (this == last)
		    break;
		prev = this++;
	    }
	}
    }

 error:

    if (res == 0) {
	cpuinfo->topology_size = 0;
	if (cpuinfo->topology) {
	    free(cpuinfo->topology);
	    cpuinfo->topology = NULL;
	}
	if (errno)
	    res = -errno;
	else
	    res = -EINVAL;
    }

    return res;

}

#else

static int
read_topology(erts_cpu_info_t *cpuinfo)
{
    return -ENOTSUP;
}

#endif

#if defined(__WIN32__)

int
erts_get_last_win_errno(void)
{
    switch (GetLastError()) {
    case ERROR_INVALID_FUNCTION:		return EINVAL;	/* 1	*/
    case ERROR_FILE_NOT_FOUND:			return ENOENT;	/* 2	*/
    case ERROR_PATH_NOT_FOUND:			return ENOENT;	/* 3	*/
    case ERROR_TOO_MANY_OPEN_FILES:		return EMFILE;	/* 4	*/
    case ERROR_ACCESS_DENIED:			return EACCES;	/* 5	*/
    case ERROR_INVALID_HANDLE:			return EBADF;	/* 6	*/
    case ERROR_ARENA_TRASHED:			return ENOMEM;	/* 7	*/
    case ERROR_NOT_ENOUGH_MEMORY:		return ENOMEM;	/* 8	*/
    case ERROR_INVALID_BLOCK:			return ENOMEM;	/* 9	*/
    case ERROR_BAD_ENVIRONMENT:			return E2BIG;	/* 10	*/
    case ERROR_BAD_FORMAT:			return ENOEXEC;	/* 11	*/
    case ERROR_INVALID_ACCESS:			return EINVAL;	/* 12	*/
    case ERROR_INVALID_DATA:			return EINVAL;	/* 13	*/
    case ERROR_OUTOFMEMORY:			return ENOMEM;	/* 14	*/
    case ERROR_INVALID_DRIVE:			return ENOENT;	/* 15	*/
    case ERROR_CURRENT_DIRECTORY:		return EACCES;	/* 16	*/
    case ERROR_NOT_SAME_DEVICE:			return EXDEV;	/* 17	*/
    case ERROR_NO_MORE_FILES:			return ENOENT;	/* 18	*/
    case ERROR_WRITE_PROTECT:			return EACCES;	/* 19	*/
    case ERROR_BAD_UNIT:			return EACCES;	/* 20	*/
    case ERROR_NOT_READY:			return EACCES;	/* 21	*/
    case ERROR_BAD_COMMAND:			return EACCES;	/* 22	*/
    case ERROR_CRC:				return EACCES;	/* 23	*/
    case ERROR_BAD_LENGTH:			return EACCES;	/* 24	*/
    case ERROR_SEEK:				return EACCES;	/* 25	*/
    case ERROR_NOT_DOS_DISK:			return EACCES;	/* 26	*/
    case ERROR_SECTOR_NOT_FOUND:		return EACCES;	/* 27	*/
    case ERROR_OUT_OF_PAPER:			return EACCES;	/* 28	*/
    case ERROR_WRITE_FAULT:			return EACCES;	/* 29	*/
    case ERROR_READ_FAULT:			return EACCES;	/* 30	*/
    case ERROR_GEN_FAILURE:			return EACCES;	/* 31	*/
    case ERROR_SHARING_VIOLATION:		return EACCES;	/* 32	*/
    case ERROR_LOCK_VIOLATION:			return EACCES;	/* 33	*/
    case ERROR_WRONG_DISK:			return EACCES;	/* 34	*/
    case ERROR_SHARING_BUFFER_EXCEEDED:		return EACCES;	/* 36	*/
    case ERROR_BAD_NETPATH:			return ENOENT;	/* 53	*/
    case ERROR_NETWORK_ACCESS_DENIED:		return EACCES;	/* 65	*/
    case ERROR_BAD_NET_NAME:			return ENOENT;	/* 67	*/
    case ERROR_FILE_EXISTS:			return EEXIST;	/* 80	*/
    case ERROR_CANNOT_MAKE:			return EACCES;	/* 82	*/
    case ERROR_FAIL_I24:			return EACCES;	/* 83	*/
    case ERROR_INVALID_PARAMETER:		return EINVAL;	/* 87	*/
    case ERROR_NO_PROC_SLOTS:			return EAGAIN;	/* 89	*/
    case ERROR_DRIVE_LOCKED:			return EACCES;	/* 108	*/
    case ERROR_BROKEN_PIPE:			return EPIPE;	/* 109	*/
    case ERROR_DISK_FULL:			return ENOSPC;	/* 112	*/
    case ERROR_INVALID_TARGET_HANDLE:		return EBADF;	/* 114	*/
    case ERROR_WAIT_NO_CHILDREN:		return ECHILD;	/* 128	*/
    case ERROR_CHILD_NOT_COMPLETE:		return ECHILD;	/* 129	*/
    case ERROR_DIRECT_ACCESS_HANDLE:		return EBADF;	/* 130	*/
    case ERROR_NEGATIVE_SEEK:			return EINVAL;	/* 131	*/
    case ERROR_SEEK_ON_DEVICE:			return EACCES;	/* 132	*/
    case ERROR_DIR_NOT_EMPTY:			return ENOTEMPTY;/* 145	*/
    case ERROR_NOT_LOCKED:			return EACCES;	/* 158	*/
    case ERROR_BAD_PATHNAME:			return ENOENT;	/* 161	*/
    case ERROR_MAX_THRDS_REACHED:		return EAGAIN;	/* 164	*/
    case ERROR_LOCK_FAILED:			return EACCES;	/* 167	*/
    case ERROR_ALREADY_EXISTS:			return EEXIST;	/* 183	*/
    case ERROR_INVALID_STARTING_CODESEG:	return ENOEXEC;	/* 188	*/
    case ERROR_INVALID_STACKSEG:		return ENOEXEC;	/* 189	*/
    case ERROR_INVALID_MODULETYPE:		return ENOEXEC;	/* 190	*/
    case ERROR_INVALID_EXE_SIGNATURE:		return ENOEXEC;	/* 191	*/
    case ERROR_EXE_MARKED_INVALID:		return ENOEXEC;	/* 192	*/
    case ERROR_BAD_EXE_FORMAT:			return ENOEXEC;	/* 193	*/
    case ERROR_ITERATED_DATA_EXCEEDS_64k:	return ENOEXEC;	/* 194	*/
    case ERROR_INVALID_MINALLOCSIZE:		return ENOEXEC;	/* 195	*/
    case ERROR_DYNLINK_FROM_INVALID_RING:	return ENOEXEC;	/* 196	*/
    case ERROR_IOPL_NOT_ENABLED:		return ENOEXEC;	/* 197	*/
    case ERROR_INVALID_SEGDPL:			return ENOEXEC;	/* 198	*/
    case ERROR_AUTODATASEG_EXCEEDS_64k:		return ENOEXEC;	/* 199	*/
    case ERROR_RING2SEG_MUST_BE_MOVABLE:	return ENOEXEC;	/* 200	*/
    case ERROR_RELOC_CHAIN_XEEDS_SEGLIM:	return ENOEXEC;	/* 201	*/
    case ERROR_INFLOOP_IN_RELOC_CHAIN:		return ENOEXEC;	/* 202	*/
    case ERROR_FILENAME_EXCED_RANGE:		return ENOENT;	/* 206	*/
    case ERROR_NESTING_NOT_ALLOWED:		return EAGAIN;	/* 215	*/
    case ERROR_NOT_ENOUGH_QUOTA:		return ENOMEM;	/* 1816	*/
    default:					return EINVAL;
    }
}

#endif