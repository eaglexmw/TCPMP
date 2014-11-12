/**********
 * Copyright (c) 2004 Greg Parker.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY GREG PARKER ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **********/

#include <PalmOS.h>
#include <PalmOSGlue.h>
#include <PceNativeCall.h>
#include <stdint.h>
#include <stddef.h>

#include "peal.h"

#include "elf.h"

#define FTRID_STEP		64

typedef struct {
    uint32_t vm_addr;
    uint32_t block;
    MemHandle resource;
    Boolean is_ftr;
	Boolean is_lib;
} SectionInfo;

struct PealModule {
	size_t size;
	Boolean is_ftr;
	Boolean is_memsemaphore;

    const Elf32_Ehdr *ehdr;

	UInt32 prgId;
	UInt16 ftrId;

    int symcount;
    const Elf32_Sym *syms;     // real address of .symtab section
    const char *strings;       // real address of .strtab section
    uintptr_t got;       // real address of .got section

    uintptr_t rw_block;  // heap block containing rw_start (unaligned)
    uintptr_t rw_start;  // real address of all r/w memory (aligned)

    uintptr_t stub;      // PealArmStub()

    SectionInfo sinfo[1];// real addresses of each section
};


// Must match struct PealArgs in arm/pealstub.c
typedef struct {
    void *fn;
    void *arg;
    void *got;
} PealArgs;


static inline uint32_t swap32(uint32_t v)
{
    return (
            ((v & 0x000000ff) << 24) |
            ((v & 0x0000ff00) << 8) |
            ((v & 0x00ff0000) >> 8) |
            ((v & 0xff000000) >> 24)
            );
}


static inline uint16_t swap16(uint16_t v)
{
    return (
            ((v & 0x00ff) << 8) |
            ((v & 0xff00) >> 8)
            );
}


static inline void swap_ehdr(Elf32_Ehdr *ehdr)
{
    ehdr->e_type      = swap16(ehdr->e_type);
    ehdr->e_machine   = swap16(ehdr->e_machine);
    ehdr->e_version   = swap32(ehdr->e_version);
    ehdr->e_entry     = swap32(ehdr->e_entry);
    ehdr->e_phoff     = swap32(ehdr->e_phoff);
    ehdr->e_shoff     = swap32(ehdr->e_shoff);
    ehdr->e_flags     = swap32(ehdr->e_flags);
    ehdr->e_ehsize    = swap16(ehdr->e_ehsize);
    ehdr->e_phentsize = swap16(ehdr->e_phentsize);
    ehdr->e_phnum     = swap16(ehdr->e_phnum);
    ehdr->e_shentsize = swap16(ehdr->e_shentsize);
    ehdr->e_shnum     = swap16(ehdr->e_shnum);
    ehdr->e_shstrndx  = swap16(ehdr->e_shstrndx);
}


static uint16_t peek16(uintptr_t block, uintptr_t dst)
{
    block = block; 
    return swap16(*(uint16_t *)dst);
}

static uint32_t peek32(uintptr_t block, uintptr_t dst)
{
    block = block; 
    return swap32(*(uint32_t *)dst);
}

static void poke16(PealModule *m,uintptr_t block, uintptr_t dst, uint16_t value)
{
    ptrdiff_t offset = dst - block;
    value = swap16(value);
	if (*(uint16_t *)dst != value)
	{
		if (!m->is_memsemaphore && errNone == DmWriteCheck((void *)block, offset, sizeof(value))) {
			DmWrite((void *)block, offset, &value, sizeof(value));
		} else {
			*(uint16_t *)dst = value;
		}
	}
}

static void poke32(PealModule *m,uintptr_t block, uintptr_t dst, uint32_t value)
{
    ptrdiff_t offset = dst - block;
    value = swap32(value);
	if (*(uint32_t *)dst != value)
	{
		if (!m->is_memsemaphore && errNone == DmWriteCheck((void *)block, offset, sizeof(value))) {
			DmWrite((void *)block, offset, &value, sizeof(value));
		} else {
			*(uint32_t *)dst = value;
		}
	}
}


static const Elf32_Shdr *section_for_index(const Elf32_Ehdr *ehdr, uint16_t index)
{
    Elf32_Shdr *shdr;
    if (index >= swap16(ehdr->e_shnum)) return NULL;
    shdr = (Elf32_Shdr *)(((uint8_t *)ehdr)+swap32(ehdr->e_shoff));
    return shdr+index;
}


static const char *section_name(const PealModule *m, const Elf32_Shdr *shdr)
{
    // NOT m->strings!
    const char *strings = (const char *)m->sinfo[swap16(m->ehdr->e_shstrndx)].vm_addr;
    return strings + swap32(shdr->sh_name);
}


static const char *symbol_name(const PealModule *m, const Elf32_Sym *sym)
{
    return m->strings + swap32(sym->st_name);
}


static const Elf32_Sym *symbol_lookup(const PealModule *m, const char *query, int hint)
{
	const Elf32_Sym *end;
	const Elf32_Sym *sym;
    int i;

    if (!m->syms  ||  !m->strings  ||  !query) return NULL;  // sorry

	if (hint >= m->symcount)
		hint = m->symcount-1;

	sym = m->syms+hint;
	i = StrCompareAscii(symbol_name(m, sym),query);
	if (i==0)
		return sym;

	// symtab has to be ordered by peal-postlink!
	if (i<0)
	{
		end = m->syms+m->symcount;
		for (++sym;sym<end;++sym)
			if (StrCompareAscii(symbol_name(m, sym),query)==0)
				return sym;
	}
	else
	{
		end = m->syms;
		for (--sym;sym>=end;--sym)
			if (StrCompareAscii(symbol_name(m, sym),query)==0)
				return sym;
	}
	return NULL;
}


// ELF file behaves like a .o, so st_value is an in-section offset
#define thumb 1
static uintptr_t symbol_address(const PealModule *m, const Elf32_Sym *sym, int interwork, const PealModule *common)
{
	uintptr_t address;
    int index = swap16(sym->st_shndx);

	if (m->sinfo[index].is_lib)
	{
		if (!common) return 0;
		sym = symbol_lookup(common, symbol_name(m,sym), swap32(sym->st_value) >> 2);
		if (!sym) return 0;
		m = common;
		index = swap16(sym->st_shndx);
	}

	address = (uintptr_t)(m->sinfo[index].vm_addr + swap32(sym->st_value));
	if (interwork  &&  ELF32_ST_TYPE(sym->st_info) == STT_LOPROC) {
		// symbol is a pointer to a Thumb function - use interworkable address
		address++;
	}
	return address;
}


/* 
   thunks from Thumb code:
   BL:   thunk is Thumb, jumps to Thumb
   BLX:  thunk is ARM, jumps to ARM
   
   thunks from ARM code:
   Bcc:  thunk is ARM, jumps to ARM
   BLcc: thunk is ARM, jumps to ARM
   BLX:  thunk is Thumb, jumps to Thumb
*/

// 1: thumb thunk
// 0: arm thunk
// -1: bad src
static int choose_thunk(uintptr_t src, Boolean srcIsThumb)
{
    if (srcIsThumb) {
        uint16_t insn1 = swap16(*(uint16_t *)src);
        uint16_t insn2 = swap16(*(uint16_t *)(src+2));
        uint16_t opcode1 = insn1 >> 11;
        uint16_t opcode2 = insn2 >> 11;
        if (opcode1 == 0x001e  &&  opcode2 == 0x001f) {
            // BL: Thumb->Thumb branch, use Thumb thunk
            return thumb;
        } else if (opcode1 == 0x001e  &&  opcode2 == 0x001d) {
            // BLX: Thumb->ARM branch, use ARM thunk
            return 0;
        } else {
            // unknown instruction
            return -1;
        }
    } else {
        uint32_t insn = swap32(*(uint32_t *)src);
        uint32_t cond = (insn & 0xf0000000) >> 28;
        uint32_t opcode = (insn & 0x0e000000) >> 25;
        if (opcode == 0x05) {
            if (cond == 0x0f) {
                // BLX: ARM->Thumb branch, use Thumb thunk
                return thumb;
            } else {
                // Bcc, BLcc: ARM->ARM branch, use ARM thunk
                return 0;
            }
        } else {
            // unknown instruction
            return -1;
        }
    }
}


/* 
   Thumb thunk
   * 16 bytes total
   * must be 4-byte aligned with leading NOP to handle ARM BLX
   * data value is dst+1 so BX stays in Thumb mode
   * for LDR, the PC is already 4 bytes ahead, and the immediate must 
     be 4-byte aligned.

 code (little-endian):
   00 46C0  NOP 
   02 B401  PUSH r0
   04 4801  LDR r0, [PC, #4]
   06 4684  MOV r12, r0
   08 BC01  POP r0
   0a 4760  BX  r12
 data:
   0c 4-byte dst+1  (needs +1 so BX stays in Thumb mode)
*/

/*
   ARM thunk
   * 8 bytes total
   * for LDR, the PC is already 8 bytes ahead
   * `LDR PC, ...` on ARMv5T will switch to Thumb mode if dest bit 0 is set, 
     but that should never happen here.

 code (big-endian):
   00 E51FF004  LDR PC, [PC, #-4]
 data:
   04 4-byte dst   
*/


typedef struct {
    uintptr_t thunk;
    uintptr_t dst;
} thunk_desc_t;

// thunkList[0] is ARM, thunkList[1] is Thumb
static thunk_desc_t *thunkList[2] = {NULL, NULL};
static unsigned int thunksUsed[2] = {0, 0};
static unsigned int thunksAllocated[2] = {0, 0};


// arch 1 == Thumb, arch 0 == ARM
static thunk_desc_t *find_thunk_desc(uintptr_t dst, int arch)
{
    thunk_desc_t *desc;
    unsigned int i;

    // Search for existing thunk.
    for (i = 0; i < thunksUsed[arch]; i++) {
        desc = &thunkList[arch][i];
        if (desc->dst == dst) return desc;
    }

    // No match. Make new thunk descriptor.
    if (thunksUsed[arch] == thunksAllocated[arch]) {
        thunk_desc_t *newList;
        thunksAllocated[arch] = thunksAllocated[arch] * 2 + 8;
        newList = MemPtrNew(thunksAllocated[arch] * sizeof(thunk_desc_t));
        if (!newList) return NULL;
        MemSet(newList, thunksAllocated[arch] * sizeof(thunk_desc_t), 0);
        if (thunkList[arch]) {
            MemMove(newList, thunkList[arch], 
                    thunksUsed[arch] * sizeof(thunk_desc_t));
            MemPtrFree(thunkList[arch]);
        }
        thunkList[arch] = newList;
    }
    desc = &thunkList[arch][thunksUsed[arch]++];
    desc->thunk = 0;
    desc->dst = dst;
    return desc;
}


static void free_thunk_descs(void)
{
    if (thunkList[0]) MemPtrFree(thunkList[0]);
    if (thunkList[1]) MemPtrFree(thunkList[1]);
    thunkList[0] = thunkList[1] = NULL;
    thunksUsed[0] = thunksUsed[1] = 0;
    thunksAllocated[0] = thunksAllocated[1] = 0;
}


static Boolean insn_is_bx(uint16_t insn)
{
    uint16_t opcode = insn >> 7;  // keep prefix+opcode+H1; discard H2+regs

    return (opcode == 0x8e);  // 010001 11 0 x xxx xxx
}


static uintptr_t generate_thunk(PealModule *m,uintptr_t *thunks, 
                                uintptr_t src_block, uintptr_t src, 
                                uintptr_t dst, int srcIsThumb)
{
    int thunkIsThumb;
    thunk_desc_t *desc;

    // Examine the instruction at *src to choose which thunk type we need.
    thunkIsThumb = choose_thunk(src, srcIsThumb);
    if (thunkIsThumb < 0) return 0;

    // Look for an existing thunk with the same dst and instruction set, 
    // and make a new one if necessary
    desc = find_thunk_desc(dst, thunkIsThumb);
    if (!desc) return 0; // out of memory
    if (desc->thunk == 0) {
        // New thunk.
        if (thunkIsThumb) {
            uint16_t target_insn = swap16(*(uint16_t *)dst);
            desc->thunk = (*thunks -= 16);
            if (insn_is_bx(target_insn)) {
                // Branch target insn at dst is another bx (e.g. call_via_rX 
                // glue). Simply copy that instruction into the stub.
                // This is necessary for correctness - the Thumb thunk 
                // interferes with call_via_ip - and also runs faster.
                // fixme need to handle BLX too?
                poke16(m,src_block, desc->thunk +  0, target_insn);
            } else {
                // Normal Thumb thunk.
                poke16(m,src_block, desc->thunk +  0, 0x46C0); // NOP
                poke16(m,src_block, desc->thunk +  2, 0xB401); // PUSH r0
                poke16(m,src_block, desc->thunk +  4, 0x4801); // LDR  r0,[PC,#4]
                poke16(m,src_block, desc->thunk +  6, 0x4684); // MOV  r12, r0
                poke16(m,src_block, desc->thunk +  8, 0xBC01); // POP  r0
                poke16(m,src_block, desc->thunk + 10, 0x4760); // BX   r12
                poke32(m,src_block, desc->thunk + 12, dst | 1);
            }
        } else {
            desc->thunk = (*thunks -= 8);
            poke32(m,src_block, desc->thunk + 0, 0xE51FF004); // LDR PC, [PC,#-4]
            poke32(m,src_block, desc->thunk + 4, dst);
        }
    }

    return desc->thunk;
}


static PealModule *allocate(const Elf32_Ehdr *ehdr)
{
    size_t size = sizeof(SectionInfo) * (swap16(ehdr->e_shnum)-1) + sizeof(PealModule);
    PealModule *m = MemPtrNew(size);
    if (!m) return NULL;

    // fixme sanity-check ehdr

    MemSet(m, size, 0);
	m->size = size;
    m->ehdr = ehdr;
    return m;
}


static void cleanup(PealModule *m)
{
    free_thunk_descs();

    if (m) {
        unsigned int i, count = swap16(m->ehdr->e_shnum);
		for (i = 0; i < count; i++) {
            if (m->sinfo[i].resource) {
                MemHandleUnlock(m->sinfo[i].resource);
                DmReleaseResource(m->sinfo[i].resource);
            } else if (m->sinfo[i].is_ftr) {
				FtrPtrFree(m->prgId,m->ftrId+i);
            }
        }
        if (m->rw_block) MemPtrFree((void *)m->rw_block);
		if (m->is_ftr)
			FtrPtrFree(m->prgId,m->ftrId-1);
		else
			MemPtrFree(m);
    }
}


static Boolean load(PealModule *m,const PealModule *common)
{
    const Elf32_Shdr *shdr;
    const Elf32_Sym *stub_sym;
    uintptr_t rw_sh_addr;
    uintptr_t rw_sh_end;
    ptrdiff_t rw_slide;
    unsigned int i;
    uint32_t max_align = 1;

    // find extent of r/w memory
    rw_sh_addr   = 0xffffffff;
    rw_sh_end    = 0;
    for (i = 0; (shdr = section_for_index(m->ehdr, i)); i++) {
        uint32_t sh_flags = swap32(shdr->sh_flags);
        uint32_t sh_addr = swap32(shdr->sh_addr);
        uint32_t sh_size = swap32(shdr->sh_size);
        uint32_t sh_type = swap32(shdr->sh_type);
        uint32_t sh_addralign = swap32(shdr->sh_addralign);

        if ((sh_flags & SHF_ALLOC) && 
            (sh_type == SHT_PROGBITS || sh_type == SHT_NOBITS)) 
        {
            if ((sh_flags & SHF_WRITE)  &&  sh_addr < rw_sh_addr) {
                rw_sh_addr = sh_addr;
            }
            if ((sh_flags & SHF_WRITE)  &&  sh_addr + sh_size > rw_sh_end) {
                rw_sh_end = sh_addr + sh_size;
            }
            if (sh_addralign > max_align) {
                max_align = sh_addralign;
            }
        }
    }


    // allocate r/w memory
    if (rw_sh_addr == 0xffffffff  ||  rw_sh_addr == rw_sh_end) {
        m->rw_block = 0;
        m->rw_start = 0;
        rw_slide = 0;
    } else {
        // add leading pad to fix alignment in case first rw section
        // is less aligned than other rw sections.
        rw_sh_addr -= rw_sh_addr % max_align;

        // add leading pad to heap block in case max_align is 
        // more aligned than MemGluePtrNew's result.
        m->rw_block = (uintptr_t)MemGluePtrNew(rw_sh_end - rw_sh_addr + max_align);
        if (!m->rw_block) return false;
        m->rw_start = m->rw_block + (max_align - m->rw_block % max_align);
        if (m->rw_start % max_align) return false;

        rw_slide = m->rw_start - rw_sh_addr;
    }


    // populate r/w memory
    for (i = 0; (shdr = section_for_index(m->ehdr, i)); i++) {
        uint32_t sh_flags = swap32(shdr->sh_flags);
        uint32_t sh_addr = swap32(shdr->sh_addr);
        uint32_t sh_size = swap32(shdr->sh_size);
        uint32_t sh_type = swap32(shdr->sh_type);
        void *vm_addr = (void *)(sh_addr + rw_slide);

        if ((sh_flags & SHF_ALLOC)  &&  (sh_flags & SHF_WRITE)  &&  
            (sh_type == SHT_NOBITS  ||  sh_type == SHT_PROGBITS)) 
        {
            if (sh_type == SHT_NOBITS) {
                MemSet(vm_addr, sh_size, 0);  // .bss section
            } else {
                MemMove(vm_addr, (void *)m->sinfo[i].vm_addr, sh_size);
            }

            // use r/w location instead of r/o location from now on
            // If this section was in a large resource, block is a 
            // temporary heap buffer that is now freed.
            // fixme large temporary buffers suck
            if (m->sinfo[i].resource) {
                MemHandleUnlock(m->sinfo[i].resource);
                DmReleaseResource(m->sinfo[i].resource);
            } else if (m->sinfo[i].is_ftr) {
				FtrPtrFree(m->prgId,m->ftrId+i);
            }
            m->sinfo[i].block = m->rw_block;
            m->sinfo[i].vm_addr = (uintptr_t)vm_addr;
            m->sinfo[i].resource = 0;
            m->sinfo[i].is_ftr = false;
        }
    }


    // find symtab and string sections (both unique)
    m->syms = NULL;
    m->symcount = 0;
    m->strings = 0;
    for (i = 0; (shdr = section_for_index(m->ehdr, i)); i++) {
        if (swap32(shdr->sh_type) == SHT_SYMTAB) {
            m->syms = (Elf32_Sym *)m->sinfo[i].vm_addr;
            m->symcount = swap32(shdr->sh_size) / sizeof(Elf32_Sym);
        }
        if (swap32(shdr->sh_type) == SHT_STRTAB) {
            m->strings = (char *)m->sinfo[i].vm_addr;
        }
    }


    // find GOT using section named .got
    // This must be done AFTER the symtab, strtab, and slides are available
    // This must be done BEFORE relocations are performed
    m->got = 0;
    for (i = 0; (shdr = section_for_index(m->ehdr, i)); i++) {
        const char *name = section_name(m, shdr);
        if (0 == StrNCompareAscii(name, ".got", 5)) {
            m->got = m->sinfo[i].vm_addr;
        }
        if (0 == StrNCompareAscii(name, ".lib", 4)) {
            m->sinfo[i].is_lib = true;
        }
    }

	if (m->is_memsemaphore)
		MemSemaphoreReserve(1);

    // perform relocations
    // Don't use Thumb interworkable addresses for any relocation. 
    // All of these symbols should be section symbols, and any 
    // interwork mangling should have been done by peal-postlink.
    for (i = 0; (shdr = section_for_index(m->ehdr, i)); i++) {
        uint32_t sh_size = swap32(shdr->sh_size);
        uint32_t sh_type = swap32(shdr->sh_type);
        Elf32_Rela *rel, *relend;
        uint32_t dst_base;
        uintptr_t dst_block;
        size_t dst_size;
        uint32_t dst_index;
        uintptr_t dst_thunks;

        if (sh_type != SHT_RELA) continue;

        free_thunk_descs();

        rel = (Elf32_Rela *)m->sinfo[i].vm_addr;
        relend = rel + sh_size / sizeof(Elf32_Rela);
        
        dst_index = swap32(shdr->sh_info);
        dst_base = m->sinfo[dst_index].vm_addr;
        dst_block = m->sinfo[dst_index].block;
        dst_size = swap32(section_for_index(m->ehdr, dst_index)->sh_size);
        dst_thunks = dst_base + dst_size;  // assume postlinker aligned this

        for ( ; rel < relend; rel++) {
            uint32_t dst_offset;
            uint32_t sym_index;
            uintptr_t dst;
            uint32_t addend;
			uintptr_t symbol;

            dst_offset = swap32(rel->r_offset);
            sym_index = ELF32_R_SYM(swap32(rel->r_info));
            addend = swap32(rel->r_addend);
            symbol = symbol_address(m, m->syms + sym_index, 0, common);
			if (!symbol) goto fail;

            // *dst is ARM-swapped, and may be in storage memory. 
            // Use poke32() to change it.
            dst = dst_base + dst_offset;

            switch (ELF32_R_TYPE(swap32(rel->r_info))) {
            case R_ARM_PC24: {
                // *dst[0-23] = ((symbol + addend - dst) - 8) / 4
                // value must be SIGNED!
                int32_t value = symbol + addend - (uintptr_t)dst;
                if (value % 4) goto fail;
                value = (value - 8) / 4;

                if (value != ((value << 8) >> 8)) {
                    // Relocation no longer fits in 24 bits. Use a thunk.
                    uintptr_t thunk = 
                        generate_thunk(m, &dst_thunks, dst_block, dst, 
                                       symbol + addend, 0);
                    if (thunk == 0) goto fail;

                    // Re-aim value at the thunk.
                    value = thunk - (uintptr_t)dst;
                    if (value % 4) goto fail;
                    value = (value - 8) / 4;
                    if (value != ((value << 8) >> 8)) goto fail;
                }

                poke32(m,dst_block, dst, 
                       (value & 0x00ffffff) | 
                       (peek32(dst_block, dst) & 0xff000000));
                break;
            }
            case R_ARM_THM_PC22: {
                // *(dst+0)[0-10] = (((symbol + addend - dst) - 4) / 2)[11-21]
                // *(dst+2)[0-10] = (((symbol + addend - dst) - 4) / 2)[0-10]
                // value must be SIGNED!
                int32_t value = symbol + addend - (uintptr_t)dst;
                if (value % 2) goto fail;
                value = (value - 4) / 2;

                if (value != ((value << 10) >> 10)) {
                    // Relocation no longer fits in 22 bits. Use a thunk.
                    uintptr_t thunk = 
                        generate_thunk(m, &dst_thunks, dst_block, dst, 
                                       (symbol+addend) & 0xfffffffe, thumb);
                    if (thunk == 0) goto fail;

                    // Re-aim value at the thunk.
                    value = thunk - (uintptr_t)dst;
                    if (value % 2) goto fail;
                    value = (value - 4) / 2;
                    if (value != ((value << 10) >> 10)) goto fail;
                }

                poke16(m,dst_block, dst+0, 
                       ((value >> 11) & 0x07ff) | 
                       (peek16(dst_block, dst+0) & 0xf800));
                poke16(m,dst_block, dst+2, 
                       ((value >>  0) & 0x07ff) | 
                       (peek16(dst_block, dst+2) & 0xf800));
                break;
            }
            case R_ARM_ABS32:
                // *dst = symbol + addend
                poke32(m,dst_block, dst, 
                       symbol + addend);
                break;

            case R_ARM_REL32:
                // *dst = symbol + addend - dst 
                poke32(m,dst_block, dst, 
                       symbol + addend - (uintptr_t)dst);
                break;

            case R_ARM_GOTOFF:
                // *dst = symbol + addend - GOT
                if (!m->got) goto fail;
                poke32(m,dst_block, dst, 
                       symbol + addend - m->got);
                break;

            default:
                break;
            }
        }
    }

	if (m->is_memsemaphore)
		MemSemaphoreRelease(1);

    // find ARM-side stub function
    stub_sym = symbol_lookup(m, "PealArmStub", 0);
    if (stub_sym) {
        // Don't use a Thumb interworkable address for the stub, 
        // because PceNativeCall can't handle it.
        m->stub = symbol_address(m, stub_sym, 0, NULL);
    } else {
        m->stub = 0;
    }

    // fixme call initializers and C++ constructors here

    free_thunk_descs();

    return true;

fail:
	if (m->is_memsemaphore)
		MemSemaphoreRelease(1);
	return false;
}


PealModule *PealLoad(void *mem)
{
    int i;
    const Elf32_Shdr *shdr;
    const Elf32_Ehdr *ehdr;
    PealModule *m;

    ehdr = (const Elf32_Ehdr *)mem;

    m = allocate(ehdr);
    if (!m) return NULL;

    // find sections (contiguous version)
    for (i = 0; (shdr = section_for_index(ehdr, i)); i++) {
        m->sinfo[i].block = (uintptr_t)mem;
        m->sinfo[i].vm_addr = ((uintptr_t)mem) + swap32(shdr->sh_offset);
    }

    if (load(m,NULL)) {
        return m;
    } else {
        cleanup(m);
        return NULL; 
    }
}

Boolean isCodeSection(const Elf32_Shdr* shdr)
{
	uint32_t sh_flags = swap32(shdr->sh_flags);
	uint32_t sh_type = swap32(shdr->sh_type);

    if (sh_type == SHT_SYMTAB ||
		sh_type == SHT_STRTAB ||
		sh_type == SHT_RELA)
		return false;

    if ((sh_flags & SHF_ALLOC)  &&  (sh_flags & SHF_WRITE)  &&  
        (sh_type == SHT_NOBITS  ||  sh_type == SHT_PROGBITS))
		return false;

	return true;
}

PealModule *PealLoadFromResources(DmResType type, DmResID baseID, const PealModule *common, UInt32 prgId, UInt16 ftrId, Boolean memDup, Boolean onlyFtr, Boolean memSema)
{
    int i;
    int resID;
    PealModule *m;
    const Elf32_Shdr *shdr;
	Boolean rom;
    MemHandle rsrcH;
    const Elf32_Ehdr *ehdr;

    rsrcH = DmGet1Resource(type, baseID);
    if (!rsrcH) return NULL;
    ehdr = (Elf32_Ehdr *)MemHandleLock(rsrcH);

    m = allocate(ehdr);
    if (!m) {
        MemHandleUnlock(rsrcH);
        DmReleaseResource(rsrcH);
        return NULL;
    }

	m->is_memsemaphore = memSema;
	rom = DmWriteCheck((void*)ehdr,0,4) != errNone;

    // find sections (resource version)
    // resource baseID+0 is ehdr+shdrs
    // additional sections are in consecutive resources
    // sections bigger than 65400 bytes are split into multiple resources
    // section 0 (SHT_NULL) has no resource
    // Use section 0's sinfo to stash ehdr's resource
    resID = baseID+1;
	m->ftrId = ++ftrId;
	m->prgId = prgId;
    m->sinfo[0].block = (uintptr_t)ehdr;
    m->sinfo[0].vm_addr = 0;
    m->sinfo[0].resource = rsrcH;

	if (memDup)
	{
		// duplicate first section (header)
		size_t resSize;
		resSize = MemHandleSize(rsrcH);

		if (FtrPtrNew(prgId,ftrId,resSize,(void**)&m->sinfo[0].block)!=errNone) {
            cleanup(m);
            return NULL;
        }
		m->ehdr = (Elf32_Ehdr *)m->sinfo[0].block;
        m->sinfo[0].vm_addr = m->sinfo[0].block;
		m->sinfo[0].is_ftr = 1;
		m->sinfo[0].resource = 0;

		DmWrite((void*)m->ehdr,0,ehdr,resSize);
        MemHandleUnlock(rsrcH);
        DmReleaseResource(rsrcH);
	}

    for (i = 1; (shdr = section_for_index(m->ehdr, i)); i++) {
        uint32_t sh_type = swap32(shdr->sh_type);
        uint32_t sh_size = swap32(shdr->sh_size);
        size_t offset = 0;
        size_t left = sh_size;

        if (sh_size==0 || sh_type==SHT_NULL || sh_type==SHT_NOBITS) {
            // empty section or .bss section - no resource expected
            // m->sinfo[i] already zeroed
            continue;
        }

        do {
            size_t resSize;
            MemHandle rsrcH = DmGet1Resource(type, resID++);
            if (!rsrcH) {
                // no resource - bail
                cleanup(m);
                return NULL;
            }
            resSize = MemHandleSize(rsrcH);
            if (resSize > left) {
                // resource too big - bail
                DmReleaseResource(rsrcH);
                cleanup(m);
                return NULL;
            } else if (resSize == sh_size && !memDup && (!rom || !isCodeSection(shdr))) {
                // resource just right - keep it
                if (m->sinfo[i].block) {
                    // oops, already concatenating
                    DmReleaseResource(rsrcH);
                    cleanup(m);
                    return NULL;
                }
                m->sinfo[i].block = (uintptr_t)MemHandleLock(rsrcH);
                m->sinfo[i].vm_addr = m->sinfo[i].block;
                m->sinfo[i].resource = rsrcH;
            } else {
                // concatenate multiple resources
                if (!m->sinfo[i].block) {
					if (i>=FTRID_STEP-1 || FtrPtrNew(prgId,ftrId+i,sh_size,(void**)&m->sinfo[i].block)!=errNone) {
                        DmReleaseResource(rsrcH);
                        cleanup(m);
                        return NULL;
                    }
                    m->sinfo[i].vm_addr = m->sinfo[i].block;
                    m->sinfo[i].is_ftr = true;
                }
				DmWrite((void*)m->sinfo[i].block,offset,MemHandleLock(rsrcH), resSize);
                MemHandleUnlock(rsrcH);
                DmReleaseResource(rsrcH);
                offset += resSize;
            }
            left -= resSize;
        } while (left > 0);
    }

    if (load(m,common)) {

		PealModule *m2;
		if (onlyFtr && FtrPtrNew(prgId,ftrId-1,m->size,(void**)&m2)==errNone) {
			m->is_ftr = 1;
			DmWrite(m2,0,m,m->size);
			MemPtrFree(m);
			m = m2;
		}

        return m;
    } else {
        cleanup(m);  // this cleanup includes rsrcH
        return NULL; 
    }
}


void *PealLookupSymbol(const PealModule *m, char *query)
{
    const Elf32_Sym *sym = symbol_lookup(m, query, 0);
    // Do return Thumb interworkable addresses to client code
    return sym ? (void *)symbol_address(m, sym, thumb, NULL) : NULL;
}


uint32_t PealCall(PealModule *m, void *addr, void *arg)
{
    // args does not have to be aligned; ARM side handles misalignment and swap
    PealArgs args;
    args.fn = addr;
    args.arg = arg;
    args.got = (void *)m->got;

    return PceNativeCall((NativeFuncType *)m->stub, &args);
}


void PealUnload(PealModule *m)
{
    if (m) {
        // fixme call terminators and C++ destructors here
        cleanup(m);
    }
}


