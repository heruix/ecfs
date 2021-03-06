/*
 * ECFS (Extended core file snapshot) utility (C) 2014 Ryan O'Neill
 * http://www.bitlackeys.org/#research
 * elfmaster@zoho.com
 */

#include "ecfs.h"

#define CHUNK_SIZE 40960

#define MAX_SECTIONS 1024

struct opts opts;

static ElfW(Off) get_internal_sh_offset(memdesc_t *memdesc, int type)
{
#define HEAP 0
#define STACK 1
#define VDSO 2
#define VSYSCALL 3

	int i;
	mappings_t *maps = memdesc->maps;

	switch(type) {
		case HEAP:
			for (i = 0; i < memdesc->mapcount; i++)
				if (maps[i].heap)
					return maps[i].sh_offset;
			break;
		case STACK:
			 for (i = 0; i < memdesc->mapcount; i++)
                                if (maps[i].stack)
                                        return maps[i].sh_offset;
                        break;
		case VDSO:
			 for (i = 0; i < memdesc->mapcount; i++)
                                if (maps[i].vdso) {
                                        return maps[i].sh_offset;
				}
                        break;
		case VSYSCALL:
			 for (i = 0; i < memdesc->mapcount; i++)
                                if (maps[i].vsyscall)
                                        return maps[i].sh_offset;
                        break;
		default:
			return 0;
	}
	return 0;
}

static int get_maps(pid_t pid, mappings_t *maps, const char *path)
{
	char mpath[256], buf[256], tmp[256], *p, *q = alloca(32);
	FILE *fd;
	int lc, i;
	
	snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
	
	if ((fd = fopen(mpath, "r")) == NULL) 
		return -1;

	for (lc = 0; (fgets(buf, sizeof(buf), fd) != NULL); lc++) {
		strcpy(tmp, buf); //tmp and buf are same sized buffers
		p = strchr(buf, '-');
		*p = '\0';
		p++;
		maps[lc].elfmap = 0;
		maps[lc].base = strtoul(buf, NULL, 16);
		maps[lc].size = strtoul(p, NULL, 16) - maps[lc].base;
		if (strstr(tmp, path)) {
			if (!strstr(tmp, "---p"))
				maps[lc].filename = xstrdup(strchr(tmp, '/'));
				maps[lc].elfmap++;
		}
		else
		if (strstr(tmp, "[heap]")) 
			maps[lc].heap++;
		else
		if (strstr(tmp, "[stack]"))
			maps[lc].stack++;
		else
		if (strstr(tmp, "[stack:")) { /* thread stack */
			for (i = 0, p = strchr(tmp, ':') + 1; *p != ']'; p++, i++)
				q[i] = *p;
			maps[i].thread_stack++;
			maps[i].stack_tid = atoi(q);
		}
		else 
		if (strstr(tmp, "---p")) 
			maps[lc].padding++;
		else
		if (strstr(tmp, "[vdso]")) 
			maps[lc].vdso++; 
		else
		if (strstr(tmp, "[vsyscall]"))
			maps[lc].vsyscall++;
		else
		if ((p = strrchr(tmp, '/'))) {
			if (strstr(p, ".so")) {
				maps[lc].shlib++;
				maps[lc].filename = xstrdup(strchr(tmp, '/'));
			}
			else
			if (strstr(p, "rwxp") || strstr(p, "r-xp")) {
				maps[lc].filename = xstrdup(strchr(tmp, '/'));
				maps[lc].filemap_exe++; // executable file mapping
			}
			else {
				maps[lc].filename = xstrdup(strchr(tmp, '/'));
				maps[lc].filemap++; // regular file mapping
			}	
		} else
		if (strstr(tmp, "rwxp") || strstr(tmp, "r-xp")) 
			maps[lc].anonmap_exe++; // executable anonymous mapping
		
		/* 	
		 * Set segment permissions (Or is it a special file?)
	 	 */
		if (strstr(tmp, "r--p")) 
			maps[lc].p_flags = PF_R;
		else
		if (strstr(tmp, "rw-p"))
			maps[lc].p_flags = PF_R|PF_W;
		else
		if (strstr(tmp, "-w-p"))
			maps[lc].p_flags = PF_W;
		else
		if (strstr(tmp, "--xp"))
			maps[lc].p_flags = PF_X;
		else
		if (strstr(tmp, "r-xp"))
			maps[lc].p_flags = PF_X|PF_R;
		else
		if (strstr(tmp, "-wxp"))
			maps[lc].p_flags = PF_X|PF_W;
		else
		if (strstr(tmp, "rwxp"))
			maps[lc].p_flags = PF_X|PF_W|PF_R;
		else
		if (strstr(tmp, "r--s"))
			maps[lc].special++;
		else
		if (strstr(tmp, "rw-s"))
			maps[lc].special++;
		else
		if (strstr(tmp, "-w-s"))
			maps[lc].special++;
		else
		if (strstr(tmp, "--xs"))
			maps[lc].special++;
		else
		if (strstr(tmp, "r-xs"))
			maps[lc].special++;
		else
		if (strstr(tmp, "-wxs"))
			maps[lc].special++;
		else
		if (strstr(tmp, "rwxs"))
			maps[lc].special++;
		
	}
	fclose(fd);

	return 0;
}
	
static int get_map_count(pid_t pid)
{
	FILE *pd;
	char cmd[256], buf[256];
	int lc;
	
	snprintf(cmd, sizeof(cmd), "/usr/bin/wc -l /proc/%d/maps", pid);
	if ((pd = popen(cmd, "r")) == NULL)
		return -1;
	fgets(buf, sizeof(buf), pd);
	lc = atoi(buf);
	pclose(pd);
	return lc;
}
	
/*
 * After each is dumped lets free up the memory mappings
 * before storing the snapshot descriptor in a linked list.
 */
int free_snapshot_maps(memdesc_t *memdesc)
{
	int i;
	
	for (i = 0; i < memdesc->mapcount; i++) 
		if (memdesc->maps[i].mem) 
			munmap(memdesc->maps[i].mem, memdesc->maps[i].size);
}

static int proc_status(pid_t pid, memdesc_t *memdesc)
{
	FILE *fd;
	char path[256], buf[256], *p, *tp;
	int i;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	if ((fd = fopen(path, "r")) == NULL)
		return -1;
	
	while (fgets(buf, sizeof(buf), fd)) {
		p = strchr(buf, ':');
		*p++ = '\0';
		while (*p == 0x20 || *p == '\t')
			p++;
		if (strcasecmp(buf, "name") == 0) {
			memdesc->comm = strdup(p);
			if ((tp = strchr(memdesc->comm, '\n')))
				*tp = '\0';
		}
		else
		if (strcasecmp(buf, "ppid") == 0)
			memdesc->task.leader = atoi(p);
		else
		if (strcasecmp(buf, "uid") == 0)
			memdesc->task.uid = atoi(p);
		else
		if (strcasecmp(buf, "gid") == 0)
			memdesc->task.gid = atoi(p);
		else
		if (strcasecmp(buf, "tracerpid") == 0) {
			memdesc->task.tracer = atoi(p); 
			if (memdesc->task.tracer)
				memdesc->task.state |= PS_TRACED;
		} 
		else
		if (strcasecmp(buf, "state") == 0) {
			switch(*p) {
				case 'D': 
					memdesc->task.state |= PS_SLEEP_UNINTER;
					break;
				case 'R':
					memdesc->task.state |= PS_RUNNING;
					break;
				case 'S':
					memdesc->task.state |= PS_SLEEP_INTER;
					break;
				case 'T':
					memdesc->task.state |= PS_STOPPED;
					break;
				case 'Z':
					memdesc->task.state |= PS_DEFUNCT;
					break; 
				default:
					memdesc->task.state |= PS_UNKNOWN;	
					break;
			}
		}
		
	}

	return 0;
}

char * get_exe_path(pid_t pid, const char *name)
{
	FILE *fd;
	char buf[256];
	char mpath[256];
	char *p, *ret = NULL;
	
	snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
	
	if ((fd = fopen(mpath, "r")) == NULL)
		return NULL;
	while (fgets(buf, sizeof(buf), fd)) {
		if ((p = strrchr(buf, '/')) == NULL)
			continue;
		p++;
		if (strncmp(p, name, strlen(name)) == 0) {	
			p = strchr(buf, '/');
			ret = strdup(p);	
			if ((p = strchr(ret, '\n')))
				*p = '\0';
			break;
		}
	}
	return ret;
}
	
#define UNKNOWN_SHDR_SIZE 64 // 

memdesc_t * take_process_snapshot(pid_t pid)
{
	memdesc_t *memdesc = (memdesc_t *)heapAlloc(sizeof(memdesc_t));
	int i;
	
	memset((void *)memdesc, 0, sizeof(memdesc_t));
	
	printf("[+] Taking snapshot of process %d\n", pid);

	memdesc->mapcount = get_map_count(pid);
	if (memdesc->mapcount < 0) {
		printf("[!] failed to get mapcount from /proc/%d/maps\n", pid);
		return NULL;
	}
	memdesc->maps = (mappings_t *)heapAlloc(sizeof(mappings_t) * memdesc->mapcount);
	
 	memset((void *)memdesc->maps, 0, sizeof(mappings_t) * memdesc->mapcount);
    	
	if (proc_status(pid, memdesc) < 0) {
		printf("[!] failed to get data from /proc/%d/status\n", pid);
                return NULL;
        }
	
        if ((memdesc->path = get_exe_path(pid, memdesc->comm)) == NULL) {
                printf("[!] Unable to find executable file path associated with pid: %d\n", pid);
                return NULL;
        }

	if (get_maps(pid, memdesc->maps, memdesc->path) < 0) {
		printf("[!] failed to get data from /proc/%d/maps\n", pid);
		return NULL;
	}
	
	memdesc->task.pid = memdesc->pid = pid;
	
	if (pid_attach_direct(pid) < 0) {
		printf("[!] Unable to attach to %d: %s\n", pid, strerror(errno));
		if (memdesc->task.state & PS_TRACED) 
			printf("[!] Process %d is already being traced by process %d. Kill the tracing process and try again\n", 
				memdesc->task.pid, memdesc->task.tracer);
		return NULL;
	}
	
	if (ptrace(PTRACE_GETREGS, pid, 0, (void *)&memdesc->pt_regs) < 0) {
		printf("[!] Unable to get register state-> ptrace(): %s\n", strerror(errno));
		return NULL;
	}
	
	/*
	 * XXX Set these to real values!
	 */
	memdesc->saved_auxv = (uint8_t *)heapAlloc(256);
	memdesc->stack_args = heapAlloc(80);
	memdesc->stack_args_len = 0;

 	for (i = 0; i < memdesc->mapcount; i++) {
		if (memdesc->maps[i].stack) {
			memdesc->stack.base = memdesc->maps[i].base;	
			memdesc->stack.size = memdesc->maps[i].size;
		} else
		if (memdesc->maps[i].heap) {
			memdesc->heap.base = memdesc->maps[i].base;
			memdesc->heap.size = memdesc->maps[i].size;
		} 
		
	}
	for (i = 0; i < memdesc->mapcount; i++) {
		if (memdesc->maps[i].padding || memdesc->maps[i].special)
			continue;
		memdesc->maps[i].mem = mmap(NULL, memdesc->maps[i].size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); 
		if (memdesc->maps[i].mem == MAP_FAILED) {
			perror("mmap");
			exit(-1);
		}
		/*
	 	 * Is this madvise really helping?
	 	 */
		if (madvise(memdesc->maps[i].mem, memdesc->maps[i].size, MADV_HUGEPAGE) < 0) {
			printf("madvise failed; this may result in performance degradation during snapshot creation\n");
		}

		if (pid_read(pid, (void *)memdesc->maps[i].mem, (void *)memdesc->maps[i].base, memdesc->maps[i].size) < 0) 
			printf("[!] Unable to read mapping region %lx: %s\n", memdesc->maps[i].base, strerror(errno));
	}
		
	for (i = 0; i < memdesc->mapcount; i++) {
		if (memdesc->maps[i].heap) {
			memdesc->heap.base = memdesc->maps[i].base;
			memdesc->heap.size = memdesc->maps[i].size;
		} else
		if (memdesc->maps[i].stack) {
			memdesc->stack.base = memdesc->maps[i].base;
			memdesc->stack.size = memdesc->maps[i].size;
		} else
		if (memdesc->maps[i].vdso) {
			memdesc->vdso.base = memdesc->maps[i].base;
			memdesc->vdso.size = memdesc->maps[i].size;
		} else
		if (memdesc->maps[i].vsyscall) {
			memdesc->vsyscall.base = memdesc->maps[i].base;
			memdesc->vsyscall.size = memdesc->maps[i].size;
		} 
	}
	pid_detach_direct(pid);
	
	return memdesc;
}


int dump_process_snapshot(desc_t *desc)
{
	int fd, ofd, i = 0, j, k, text_index, data_index1, data_index2, text_shdr_index;
	ElfW(Ehdr) *ehdr;
	ElfW(Phdr) *phdr;
	/* Values within the range of the executbale itself */
	ElfW(Addr) bssVaddr, dynVaddr, relVaddr, relaVaddr, ehframeVaddr, textVaddr, o_textVaddr, dataVaddr, o_dataVaddr, gotVaddr, noteVaddr, 
			hashVaddr, initVaddr, finiVaddr, pltVaddr, dsymVaddr, dstrVaddr, interpVaddr, tlsVaddr;
	ElfW(Off) bssOff, dynOff, relOff, relaOff, noteOff, ehframeOff, textOffset, dataOffset, gotOff, hashOff, 
			initOff, finiOff, pltOff, dsymOff, dstrOff, interpOff, tlsOff;
	ElfW(Word) bssSiz, dynSiz, hashSiz, ehframeSiz, textfSize, textSize, dataSize, strSiz, pltSiz, interpSiz, tlsSiz, noteSiz, dsymSiz, dstrSiz;
	
	ElfW(Off) offset, len;
	ElfW(Dyn) *dyn;
	ElfW(Shdr) *shdr;
	ElfW(Phdr) *nphdr;
	
	uint8_t *dynseg; // XXX changed this to heap alloc below
	struct stat st;
	memdesc_t *memdesc = &desc->memory;
	uint8_t *exemem;
	unsigned long tmpval;
	char *filepath = xfmtstrdup("%s/%s.%d", desc->snapdir, memdesc->comm, memdesc->pid);
	
	do {
		if (access(filepath, F_OK) == 0) {
			free(filepath);
			filepath = xfmtstrdup("%s/%s.%d.0%i", desc->snapdir, memdesc->comm, memdesc->pid, ++i);
		} else
			break;
			
	} while(1);
		
	printf("[+] Processing data and creating snapshot file: %s\n", filepath);
	if ((fd = open(filepath, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU)) < 0) {
		perror("open");
		exit(-1);
	}
	
	for (i = 0; i < memdesc->mapcount; i++) {
		if (!memdesc->maps[i].elfmap)	
			continue;
		ehdr = (ElfW(Ehdr) *)memdesc->maps[i].mem;
		text_index = i;
		data_index1 = text_index + 1; 
		data_index2 = text_index + 2;
		break;
	}
	switch(ehdr->e_type) {
                case ET_EXEC:
                        desc->exe_type = ET_EXEC;
                        break;
                case ET_DYN:
                        desc->exe_type = ET_DYN;
                        break;
                default:
                        desc->exe_type = ET_NONE;
        }

	phdr = (ElfW(Phdr) *)&memdesc->maps[i].mem[ehdr->e_phoff];
	for (i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                        if (phdr[i].p_offset == 0 && (phdr[i].p_flags & PF_X)) {
                                /* text segment */
                                memdesc->base = o_textVaddr = textVaddr = phdr[i].p_vaddr;
                                textOffset = phdr[i].p_offset;
                                textSize = phdr[i].p_memsz;
                                if (desc->exe_type == ET_DYN) 
                                        textVaddr += memdesc->maps[text_index].base;
                        } else
                        if (phdr[i].p_offset != 0 && (phdr[i].p_flags & PF_W)) {
                                /* data segment */
                                o_dataVaddr = dataVaddr = phdr[i].p_vaddr;
                                dataOffset = phdr[i].p_offset;
                                dataSize = phdr[i].p_memsz;
                                if (desc->exe_type == ET_DYN)
                                        dataVaddr += memdesc->maps[text_index].base;
				memdesc->data_base = dataVaddr;
				bssOff = dataOffset + dataSize;
				bssVaddr = dataVaddr + dataSize;
				bssSiz = phdr[i].p_memsz - phdr[i].p_filesz;
                                break;
                        }
                }
        }

	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type == PT_NOTE) {
			noteVaddr = phdr[i].p_vaddr + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
			noteOff = phdr[i].p_offset;
			noteSiz = phdr[i].p_memsz;
		} else
		if (phdr[i].p_type == PT_GNU_EH_FRAME) {
			ehframeVaddr = phdr[i].p_vaddr + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
			ehframeOff = phdr[i].p_offset;
			ehframeSiz = phdr[i].p_memsz;
		} else
		if (phdr[i].p_type == PT_INTERP) {
			interpVaddr = phdr[i].p_vaddr + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
			interpOff = phdr[i].p_offset;
			interpSiz = phdr[i].p_memsz;
		} else
		if (phdr[i].p_type == PT_TLS) {
			tlsVaddr = phdr[i].p_vaddr + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
			tlsOff = phdr[i].p_offset;
			tlsSiz = phdr[i].p_memsz;
		} else
		if (phdr[i].p_type == PT_DYNAMIC) {
			desc->dynlinking++;
			dynVaddr = phdr[i].p_vaddr + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
			dynOff = phdr[i].p_offset;
			dynSiz = phdr[i].p_filesz;
			pid_attach_direct(memdesc->pid); 
			ElfW(Addr) dvaddr = phdr[i].p_vaddr + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
			dynseg = (uint8_t *)heapAlloc(phdr[i].p_memsz);
			pid_read(memdesc->pid, (void *)dynseg, (void *)dvaddr, phdr[i].p_memsz);
			pid_detach_direct(memdesc->pid);
			dyn = (ElfW(Dyn) *)dynseg;
		        for (j = 0; dyn[j].d_tag != DT_NULL; j++) {
				switch(dyn[j].d_tag) {
					case DT_REL:
						relVaddr = dyn[j].d_un.d_val;
						relOff = relVaddr - textVaddr;
						break;
					case DT_RELA:
						relaVaddr = dyn[j].d_un.d_val;
						relaOff = relaVaddr - textVaddr;
						break;
                                 	case DT_PLTGOT:
                                        	gotVaddr = dyn[j].d_un.d_val;
						gotOff = dyn[j].d_un.d_val - dataVaddr;
						gotOff += (ElfW(Off))dataOffset;
                                                break;
					case DT_GNU_HASH:
						hashVaddr = dyn[j].d_un.d_val;
						hashOff = hashVaddr - textVaddr;
						break;
					case DT_INIT: 
						initVaddr = dyn[j].d_un.d_val + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
						initOff = initVaddr - textVaddr;
						break;
					case DT_FINI:
						finiVaddr = dyn[j].d_un.d_val + (desc->exe_type == ET_EXEC ? 0 : textVaddr);
						finiOff = finiVaddr - textVaddr;
						break;
					case DT_STRSZ:
						strSiz = dyn[j].d_un.d_val;
						break;	
                                        case DT_PLTRELSZ:
                                                pltSiz = dyn[j].d_un.d_val;
                                                break;
                                        case DT_SYMTAB:
                                                dsymVaddr = dyn[j].d_un.d_ptr;
						dsymOff = dsymVaddr - textVaddr;
                                                break;
                                        case DT_STRTAB:
                                                dstrVaddr = dyn[j].d_un.d_ptr;
						dstrOff = dstrVaddr - textVaddr;
                                                break;
					
                                }
			}
		}
	}
                               
	size_t bytes_written = 0;
	
	nphdr = (ElfW(Phdr) *)heapAlloc((memdesc->mapcount + ehdr->e_phnum) * sizeof(ElfW(Phdr)));
	for (i = 0; i < ehdr->e_phnum; i++)
		memcpy((void *)&nphdr[i], (void *)&phdr[i], sizeof(ElfW(Phdr)));
	for (k = 0, j = 0; j < memdesc->mapcount; j++) {
		if (memdesc->maps[j].padding)
			continue;
		if (memdesc->maps[j].elfmap)
			continue;
		if (memdesc->maps[j].special)
			continue;
		nphdr[i + k].p_type = PT_LOAD;
		nphdr[i + k].p_vaddr = memdesc->maps[j].base;
		nphdr[i + k].p_paddr = memdesc->maps[j].base;
		nphdr[i + k].p_memsz = memdesc->maps[j].size;
		nphdr[i + k].p_filesz = memdesc->maps[j].size;
		nphdr[i + k].p_offset = 0;
		nphdr[i + k].p_flags = memdesc->maps[j].p_flags;
		k++;
	}
	int n_phnum = i + k;

	/* How much bigger is the new phdr table from the old? */
	uint32_t pdiff = ((sizeof(ElfW(Phdr)) * n_phnum) - sizeof(ElfW(Phdr)) * ehdr->e_phnum);
	
	/* NOTE: We must shift segments located after the phdr table forward by pdiff bytes */
	/* XXX scratch out above note. Instead we write the phdr table to the end of the file */
	/* so adjusting offsets isn't necessary. */
	/*
	 * Write out ELF text/data segment
	 */
	size_t b;
	for (i = 0; i < memdesc->mapcount; i++) {
		if (!memdesc->maps[i].elfmap)
			continue;
		if ((textVaddr & ~(PAGE_SIZE - 1)) == memdesc->maps[i].base) { 
			/* Write out ELF file header and text */
			offset = textVaddr - memdesc->maps[i].base;
			len = memdesc->maps[i].size;
			do {
				if (len < CHUNK_SIZE) {
					b = write(fd, (char *)&memdesc->maps[i].mem[offset], len);
					bytes_written += b;
					break;
				}
				write(fd, (char *)&memdesc->maps[i].mem[offset], CHUNK_SIZE);
				offset += CHUNK_SIZE;
				bytes_written += offset;
				len -= CHUNK_SIZE;
			} while (len > 0);
		} else
		if ((dataVaddr & ~(PAGE_SIZE - 1)) == memdesc->maps[i].base) {	
			/* Write out ELF data segment */
			offset = dataVaddr - memdesc->maps[i].base;

			len = memdesc->maps[i].size;
			do {
				if (len < CHUNK_SIZE) {
					b = write(fd, (char *)&memdesc->maps[i].mem[offset], len);
					bytes_written += b;
					break;
				}
				write(fd, (char *)&memdesc->maps[i].mem[offset], CHUNK_SIZE);
                                offset += CHUNK_SIZE;
				bytes_written += offset;
                                len -= CHUNK_SIZE;
                        } while (len > 0); 
			if (memdesc->maps[i + 1].elfmap) {
				offset = 0;
				len = memdesc->maps[i + 1].size;
				do {
					if (len < CHUNK_SIZE) {
						b = write(fd, (char *)&memdesc->maps[i + 1].mem[offset], len);
						bytes_written += b;
						break;
					}
					write(fd, (char *)&memdesc->maps[i + 1].mem[offset], CHUNK_SIZE);
					offset += CHUNK_SIZE;
					bytes_written += offset;
					len -= CHUNK_SIZE;
				} while(len > 0);
			}
				
		} 
	}
	
	/*
	 * Write out rest of memory mappings:
	 * heap, stack, shared libraries, etc.
	 */
	for (i = 0; i < memdesc->mapcount; i++) {
		if (memdesc->maps[i].elfmap)
			continue;
		if (memdesc->maps[i].padding)
			continue;
		offset = 0;
		len = memdesc->maps[i].size;
		memdesc->maps[i].sh_offset = bytes_written;
		for (j = 0; j < n_phnum; j++) {
			if (phdr[j].p_vaddr == memdesc->maps[i].base)
				phdr[j].p_offset = memdesc->maps[i].sh_offset;
		}
		do {
	   		if (len < CHUNK_SIZE) {
                        	b = write(fd, (char *)&memdesc->maps[i + 1].mem[offset], len);
				bytes_written += b;
                                break;
                        }
                        write(fd, (char *)&memdesc->maps[i + 1].mem[offset], CHUNK_SIZE);
                        offset += CHUNK_SIZE;
                        len -= CHUNK_SIZE;
                } while(len > 0);
		bytes_written += offset;
		
	}
	
	close(fd);

	if ((fd = open(filepath, O_RDWR)) < 0) {
		perror("open");
		exit(-1);
	}
	
	if (fstat(fd, &st) < 0) {
		perror("fstat");
		exit(-1);
	}
	
	ElfW(Off) e_shoff = st.st_size;
	
	/*
	 * Seek to end of file so we can write shdr table
	 */
	if (lseek(fd, 0, SEEK_END) < 0) {
		perror("open");
		exit(-1);
	}
	
	
	/*
	 * Build section header table
	 */
	shdr = (ElfW(Shdr) *)alloca(sizeof(ElfW(Shdr)) * MAX_SECTIONS);
	char *StringTable = (char *)alloca(512);
	unsigned int stoffset = 0;
	int scount = 0;
	
	shdr[scount].sh_type = SHT_NULL;
	shdr[scount].sh_offset = 0;
	shdr[scount].sh_addr = 0;
	shdr[scount].sh_flags = 0;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = 0;
	shdr[scount].sh_addralign = 0;
	shdr[scount].sh_name = 0;
	strcpy(&StringTable[stoffset], "");
	stoffset += 1;
	scount++;

	/*
	 * .interp
	 */
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = interpOff;
	shdr[scount].sh_addr = interpVaddr;
	shdr[scount].sh_flags = SHF_ALLOC;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = interpSiz;
	shdr[scount].sh_addralign = 1;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".interp");
	stoffset += strlen(".interp") + 1;
	scount++;
	/*
	 * .note
	 */
	shdr[scount].sh_type = SHT_NOTE;
	shdr[scount].sh_offset = noteOff;
	shdr[scount].sh_addr = noteVaddr;
	shdr[scount].sh_flags = SHF_ALLOC;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = noteSiz;
	shdr[scount].sh_addralign = 4;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".note");
	stoffset += strlen(".note") + 1;
	scount++;

	/*
	 * .hash
	 */
	shdr[scount].sh_type = SHT_GNU_HASH; // use SHT_HASH?
	shdr[scount].sh_offset = hashOff; 
	shdr[scount].sh_addr = hashVaddr;
	shdr[scount].sh_flags = SHF_ALLOC;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = UNKNOWN_SHDR_SIZE;
	shdr[scount].sh_addralign = 4;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".hash");
	stoffset += strlen(".hash") + 1;
	scount++;

	/*
	 * .dynsym
	 */
	shdr[scount].sh_type = SHT_DYNSYM;
	shdr[scount].sh_offset = dsymOff;
	shdr[scount].sh_addr = dsymVaddr;
	shdr[scount].sh_flags = SHF_ALLOC;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = scount + 1;
	shdr[scount].sh_entsize = sizeof(ElfW(Sym));
	shdr[scount].sh_size = dstrOff - dsymOff;
	shdr[scount].sh_addralign = sizeof(long);
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".dynsym");
	stoffset += strlen(".dynsym") + 1;
	scount++;
	/*
	 * .dynstr
	 */
	shdr[scount].sh_type = SHT_STRTAB;
	shdr[scount].sh_offset = dstrOff;
	shdr[scount].sh_addr = dstrVaddr;
	shdr[scount].sh_flags = SHF_ALLOC;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = sizeof(ElfW(Sym));
	shdr[scount].sh_size = strSiz;
	shdr[scount].sh_addralign = 1;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".dynstr");
	stoffset += strlen(".dynstr") + 1;
	scount++;

	/*
	 * rela.dyn
	 */
	shdr[scount].sh_type = (__ELF_NATIVE_CLASS == 64) ? SHT_RELA : SHT_REL;
	shdr[scount].sh_offset = (__ELF_NATIVE_CLASS == 64) ? relaOff : relOff;
	shdr[scount].sh_addr = (__ELF_NATIVE_CLASS == 64) ? relaVaddr : relVaddr;
	shdr[scount].sh_flags = SHF_ALLOC;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = scount - 1;
	shdr[scount].sh_entsize = (__ELF_NATIVE_CLASS == 64) ? sizeof(Elf64_Rela) : sizeof(Elf32_Rel);
	shdr[scount].sh_size = UNKNOWN_SHDR_SIZE;
	shdr[scount].sh_addralign = sizeof(long); 
	shdr[scount].sh_name = stoffset;
	if (__ELF_NATIVE_CLASS == 64) {
		strcpy(&StringTable[stoffset], ".rela.dyn");
		stoffset += strlen(".rela.dyn") + 1;
	} else {
		strcpy(&StringTable[stoffset], ".rel.dyn");
		stoffset += strlen(".rel.dyn") + 1;
	}
	scount++;

	/*
	 * .init
	 */
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = initOff;
	shdr[scount].sh_addr = initVaddr;
	shdr[scount].sh_flags = SHF_ALLOC|SHF_EXECINSTR;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = UNKNOWN_SHDR_SIZE;
	shdr[scount].sh_addralign = sizeof(long);
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".init");
	stoffset += strlen(".init") + 1;
	scount++;
	
	/*
	 * .text
	 */
	text_shdr_index = scount;
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = textOffset;
	shdr[scount].sh_addr = textVaddr;
	shdr[scount].sh_flags = SHF_ALLOC|SHF_EXECINSTR;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = textSize;
	shdr[scount].sh_addralign = 16;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".text");
	stoffset += strlen(".text") + 1;
	scount++;

	
	/*
	 * .fini
	 */
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = finiOff;
	shdr[scount].sh_addr = finiVaddr;
	shdr[scount].sh_flags = SHF_ALLOC|SHF_EXECINSTR;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = UNKNOWN_SHDR_SIZE;
	shdr[scount].sh_addralign = 16;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".fini");
	stoffset += strlen(".fini") + 1;
	scount++;

	/*
	 * .eh_frame_hdr
	 */
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = ehframeOff;
	shdr[scount].sh_addr = ehframeVaddr;	
	shdr[scount].sh_flags = SHF_ALLOC|SHF_EXECINSTR;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = ehframeSiz;
	shdr[scount].sh_addralign = 16;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".eh_frame_hdr");
	stoffset += strlen(".eh_frame_hdr") + 1;
	scount++;
	
	/*
	 * .eh_frame
	 */
        shdr[scount].sh_type = SHT_PROGBITS;
        shdr[scount].sh_offset = ehframeOff + (ehframeSiz + 4);
        shdr[scount].sh_addr = ehframeVaddr + ehframeSiz;
        shdr[scount].sh_flags = SHF_ALLOC|SHF_EXECINSTR;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = (ElfW(Off))((ehframeVaddr + ehframeSiz) - textVaddr);
        shdr[scount].sh_addralign = 16;
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".eh_frame");
        stoffset += strlen(".eh_frame") + 1;
        scount++;

	/*
	 * .dynamic 
	 */
	shdr[scount].sh_type = SHT_DYNAMIC;
	shdr[scount].sh_offset = dynOff;
	shdr[scount].sh_addr = dynVaddr;
	shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = (__ELF_NATIVE_CLASS == 64) ? 16 : 8;
	shdr[scount].sh_size = dynSiz;
	shdr[scount].sh_addralign = sizeof(long);
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".dynamic");
	stoffset += strlen(".dynamic") + 1;
	scount++;

	/*
	 * .got.plt
	 */
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = gotOff;
	shdr[scount].sh_addr = gotVaddr;
	shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = sizeof(long);
	shdr[scount].sh_size = UNKNOWN_SHDR_SIZE;
	shdr[scount].sh_addralign = sizeof(long);
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".got.plt");
	stoffset += strlen(".got.plt") + 1;
	scount++;

	/*
 	 * .data
	 */
	shdr[scount].sh_type = SHT_PROGBITS;
	shdr[scount].sh_offset = dataOffset;
	shdr[scount].sh_addr = dataVaddr;
	shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = dataSize;
	shdr[scount].sh_addralign = sizeof(long);
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".data");
	stoffset += strlen(".data") + 1;
	scount++;

	/*
	 * .bss
	 */
        shdr[scount].sh_type = SHT_PROGBITS; // we change this to progbits cuz we want to be able to see data
        shdr[scount].sh_offset = bssOff;
        shdr[scount].sh_addr = bssVaddr;
        shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = bssSiz;
        shdr[scount].sh_addralign = sizeof(long);
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".bss");
	stoffset += strlen(".bss") + 1;
	scount++;

	/*
	 * .heap
	 */
        shdr[scount].sh_type = SHT_PROGBITS; // we change this to progbits cuz we want to be able to see data
        shdr[scount].sh_offset = get_internal_sh_offset(memdesc, HEAP);
	shdr[scount].sh_addr = memdesc->heap.base;
        shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = memdesc->heap.size;
        shdr[scount].sh_addralign = sizeof(long);
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".heap");
        stoffset += strlen(".heap") + 1;
	scount++;

	/*
	 * .stack
	 */
        shdr[scount].sh_type = SHT_PROGBITS; // we change this to progbits cuz we want to be able to see data
        shdr[scount].sh_offset = get_internal_sh_offset(memdesc, STACK);
        shdr[scount].sh_addr = memdesc->stack.base;
        shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = memdesc->stack.size;
        shdr[scount].sh_addralign = sizeof(long);
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".stack");
        stoffset += strlen(".stack") + 1;
	scount++;

	/*
	 * .vdso
	 */
        shdr[scount].sh_type = SHT_PROGBITS; // we change this to progbits cuz we want to be able to see data
        shdr[scount].sh_offset = get_internal_sh_offset(memdesc, VDSO);
	shdr[scount].sh_addr = memdesc->vdso.base;
        shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = memdesc->vdso.size;
        shdr[scount].sh_addralign = sizeof(long);
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".vdso");
        stoffset += strlen(".vdso") + 1;
	scount++;

	/*
	 * .vsyscall
	 */
        shdr[scount].sh_type = SHT_PROGBITS; // we change this to progbits cuz we want to be able to see data
        shdr[scount].sh_offset = get_internal_sh_offset(memdesc, VSYSCALL);
        shdr[scount].sh_addr = memdesc->vsyscall.base;
        shdr[scount].sh_flags = SHF_ALLOC|SHF_WRITE;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = memdesc->vsyscall.size;
        shdr[scount].sh_addralign = sizeof(long);
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".vsyscall");
        stoffset += strlen(".vsyscall") + 1;
	scount++;

	/*
	 * .symtab
	 */
        shdr[scount].sh_type = SHT_SYMTAB;
        shdr[scount].sh_offset = 0;
        shdr[scount].sh_addr = 0;
        shdr[scount].sh_flags = 0;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = scount + 1;
        shdr[scount].sh_entsize = sizeof(ElfW(Sym));
        shdr[scount].sh_size;
        shdr[scount].sh_addralign = 4;
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".symtab");
        stoffset += strlen(".symtab") + 1;
        scount++;

	/*
	 * .strtab
	 */
        shdr[scount].sh_type = SHT_STRTAB;
        shdr[scount].sh_offset = 0;
        shdr[scount].sh_addr = 0;
        shdr[scount].sh_flags = 0;
        shdr[scount].sh_info = 0;
        shdr[scount].sh_link = 0;
        shdr[scount].sh_entsize = 0;
        shdr[scount].sh_size = 0;
        shdr[scount].sh_addralign = 4;
        shdr[scount].sh_name = stoffset;
        strcpy(&StringTable[stoffset], ".strtab");
        stoffset += strlen(".strtab") + 1;
        scount++;

	/*
 	 * .shstrtab
	 */
	shdr[scount].sh_type = SHT_STRTAB;
	shdr[scount].sh_offset = e_shoff + (sizeof(ElfW(Shdr)) * (scount  + 1));
	shdr[scount].sh_addr = 0;
	shdr[scount].sh_flags = 0;
	shdr[scount].sh_info = 0;
	shdr[scount].sh_link = 0;
	shdr[scount].sh_entsize = 0;
	shdr[scount].sh_size = stoffset + strlen(".shstrtab") + 1; 
	shdr[scount].sh_addralign = 1;
	shdr[scount].sh_name = stoffset;
	strcpy(&StringTable[stoffset], ".shstrtab");
	stoffset += strlen(".shstrtab") + 1;
	scount++;
	/*
	 * We will add the actual sections for .symtab and .strtab
	 * after we write out the current section headers first and
	 * use them to retrieve symtab info from eh_frame
	 */

	int e_shstrndx = scount - 1;
	for (i = 0; i < scount; i++) 
		write(fd, (char *)&shdr[i], sizeof(ElfW(Shdr)));
	
	write(fd, (char *)StringTable, stoffset);
	fsync(fd);
	close(fd);
	
	if ((fd = open(filepath, O_RDWR)) < 0) {
		perror("open");
		exit(-1);
	}
	
	if (fstat(fd, &st) < 0) {
		perror("fstat");
		exit(-1);
	}

	uint8_t *mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		exit(-1);
	}

	ehdr = (ElfW(Ehdr) *)mem;
	phdr = (ElfW(Phdr) *)(mem + ehdr->e_phoff);
	ehdr->e_shoff = e_shoff;
	ehdr->e_shstrndx = e_shstrndx;
	ehdr->e_shnum = scount;
	ehdr->e_type = opts.coretype ? ET_CORE : ET_NONE;

	msync(mem, st.st_size, MS_SYNC);
	munmap(mem, st.st_size);

	close(fd);

	/*
 	 * Open one last time to add symtab
	 */
       if ((fd = open(filepath, O_RDWR)) < 0) {
                perror("open");
                exit(-1);
        }

        if (fstat(fd, &st) < 0) {
                perror("fstat");
                exit(-1);
        }

        mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem == MAP_FAILED) {
                perror("mmap");
                exit(-1);
        }
	
        if (lseek(fd, 0, SEEK_END) < 0) {
                perror("open");
                exit(-1);
        }
	if (desc->exe_type == ET_DYN)
		for (i = 0; i < ehdr->e_phnum; i++) // the first e_phnum phdr's need to be adjusted if executable is PIE
			nphdr[i].p_vaddr += textVaddr;

	for (i = 0; i < n_phnum; i++)
		write(fd, &nphdr[i], sizeof(ElfW(Phdr)));
	
	ehdr = (ElfW(Ehdr) *)mem;
	ehdr->e_phoff = st.st_size;
	ehdr->e_phnum = n_phnum;
	
	msync(mem, st.st_size, MS_SYNC);
        munmap(mem, st.st_size);

        close(fd);
	
	/*
	 * Now that we have written a nearly complete file, with
	 * an in-tact eh_frame and eh_frame_hdr section, we can
	 * use libdwarf to parse the call frame data and build
	 * a local symbol table of functions.
	 */
	struct fde_func_data *fndata, *fdp;
	int fncount;
	
        if ((fncount = get_all_functions(filepath, &fndata)) < 0) {
                printf("[!] get_all_functions() failed, cannot build .symtab");
        	goto done; 
	} 
	ElfW(Sym) *symtab = (ElfW(Sym) *)alloca(sizeof(ElfW(Sym)) * fncount);
        fdp = (struct fde_func_data *)fndata; 
	char *strtab = alloca(8192);
	char *sname;
	int symstroff = 0;
	int symcount = fncount;
	int dsymcount = 0;
	
	for (i = 0; i < fncount; i++) {
		symtab[i].st_value = fdp[i].addr;
		symtab[i].st_size = fdp[i].size;
	 	symtab[i].st_info = (((STB_GLOBAL) << 4) + ((STT_FUNC) & 0xf));
		symtab[i].st_other = 0;
		symtab[i].st_shndx = text_shdr_index;
		symtab[i].st_name = symstroff;
		sname = xfmtstrdup("sub_%lx", fdp[i].addr);
		strcpy(&strtab[symstroff], sname);
		symstroff += strlen(sname) + 1;
		free(sname);	
		
	}
	size_t symtab_size = fncount * sizeof(ElfW(Sym));
	/*
	 * We append symbol table sections last 
	 */
 	if ((fd = open(filepath, O_RDWR)) < 0) {
                perror("open");
                exit(-1);
        }

        if (fstat(fd, &st) < 0) {
                perror("fstat");
                exit(-1);
        }

        mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem == MAP_FAILED) {
                perror("mmap");
                exit(-1);
        }
	ehdr = (ElfW(Ehdr) *)mem;
	
        if (lseek(fd, 0, SEEK_END) < 0) {
                perror("open");
                exit(-1);
        }
	
	uint64_t symtab_offset = lseek(fd, 0, SEEK_CUR);
	for (i = 0; i < symcount; i++) 
		write(fd, (char *)&symtab[i], sizeof(ElfW(Sym))); 
				
	StringTable = (char *)&mem[shdr[ehdr->e_shstrndx].sh_offset];
	/* Write section hdr string table */
	uint64_t stloff = lseek(fd, 0, SEEK_CUR);
	write(fd, strtab, symstroff);
	shdr = (ElfW(Shdr) *)(mem + ehdr->e_shoff);
	for (i = 0; i < ehdr->e_shnum; i++) {
		if (!strcmp(&StringTable[shdr[i].sh_name], ".symtab")) {
			shdr[i].sh_offset = symtab_offset;
			shdr[i].sh_size = sizeof(ElfW(Sym)) * fncount;
		} else
		if (!strcmp(&StringTable[shdr[i].sh_name], ".strtab")) {
			shdr[i].sh_offset = stloff;
			shdr[i].sh_size = symstroff;
		} else
		if (!strcmp(&StringTable[shdr[i].sh_name], ".dynsym")) 
			dsymcount = shdr[i].sh_size / sizeof(ElfW(Sym));
			
	}
	/*
	 * We resize the global offset table now that we know how many dynamic
	 * symbols there are. The GOT has the first 3 entries reserved (Which is sizeof(long) * 3)
	 * plus the size of dsymcount longwords.
	 */
	for (i = 0; i < ehdr->e_shnum; i++) {
		if (!strcmp(&StringTable[shdr[i].sh_name], ".got.plt")) {
			shdr[i].sh_size = (dsymcount * sizeof(ElfW(Addr))) + (3 * sizeof(ElfW(Addr)));
			break;
		}
	}
	
	msync(mem, st.st_size, MS_SYNC);
	munmap(mem, st.st_size);
	close(fd);

	/*
	 * We add a new PT_NOTE area and point the old PT_NOTE towards it
	 * so that gdb can read core file process status info
	 */
	loff_t notes_offset = build_notes_area(filepath, desc);
	
        if ((fd = open(filepath, O_RDWR)) < 0) {
                perror("open");
                exit(-1);
        }

        if (fstat(fd, &st) < 0) {
                perror("fstat");
                exit(-1);
        }

        /* This final time of open(), we map it in */
        mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem == MAP_FAILED) {
                perror("mmap");
                exit(-1);
        }
	/*
	ehdr = (ElfW(Ehdr) *)mem;	
	shdr = (ElfW(Shdr) *)(mem + ehdr->e_shoff);
	StringTable = (char *)&mem[shdr[ehdr->e_shstrndx].sh_offset];
	for (i = 0; i < ehdr->e_shnum; i++) {
		if (!strncmp(&StringTable[shdr[i].sh_name], ".note", 4)) {
			shdr[i].sh_offset = notes_offset;
			shdr[i].sh_size = 0x234;
			break;
		}
	}  */
mid_failure:
	msync(mem, st.st_size, MS_SYNC); // just incase
	munmap(mem, st.st_size);
	close(fd);
	return 0;
	
done:
	if (fd > 2)
		close(fd);
	return 0;



}



