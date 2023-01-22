#include "mold.h"

namespace mold::macho {

using E = ARM64;

static u64 page(u64 val) {
  return val & 0xffff'ffff'ffff'f000;
}

static u64 page_offset(u64 hi, u64 lo) {
  u64 val = page(hi) - page(lo);
  return (bits(val, 13, 12) << 29) | (bits(val, 32, 14) << 5);
}

template <>
void StubsSection<E>::copy_buf(Context<E> &ctx) {
  ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset);

  for (i64 i = 0; i < syms.size(); i++) {
    static const ul32 insn[] = {
      0x90000010, // adrp x16, $ptr@PAGE
      0xf9400210, // ldr  x16, [x16, $ptr@PAGEOFF]
      0xd61f0200, // br   x16
    };

    static_assert(sizeof(insn) == E::stub_size);

    u64 la_addr = ctx.lazy_symbol_ptr.hdr.addr + word_size * i;
    u64 this_addr = this->hdr.addr + E::stub_size * i;

    memcpy(buf, insn, sizeof(insn));
    buf[0] |= page_offset(la_addr, this_addr);
    buf[1] |= bits(la_addr, 11, 3) << 10;
    buf += 3;
  }
}

template <>
void StubHelperSection<E>::copy_buf(Context<E> &ctx) {
  ul32 *start = (ul32 *)(ctx.buf + this->hdr.offset);
  ul32 *buf = start;

  static const ul32 insn0[] = {
    0x90000011, // adrp x17, $__dyld_private@PAGE
    0x91000231, // add  x17, x17, $__dyld_private@PAGEOFF
    0xa9bf47f0, // stp  x16, x17, [sp, #-16]!
    0x90000010, // adrp x16, $dyld_stub_binder@PAGE
    0xf9400210, // ldr  x16, [x16, $dyld_stub_binder@PAGEOFF]
    0xd61f0200, // br   x16
  };

  static_assert(sizeof(insn0) == E::stub_helper_hdr_size);
  memcpy(buf, insn0, sizeof(insn0));

  u64 dyld_private = get_symbol(ctx, "__dyld_private")->get_addr(ctx);
  buf[0] |= page_offset(dyld_private, this->hdr.addr);
  buf[1] |= bits(dyld_private, 11, 0) << 10;

  u64 stub_binder = get_symbol(ctx, "dyld_stub_binder")->get_got_addr(ctx);
  buf[3] |= page_offset(stub_binder, this->hdr.addr - 12);
  buf[4] |= bits(stub_binder, 11, 0) << 10;

  buf += 6;

  for (i64 i = 0; i < ctx.stubs.syms.size(); i++) {
    static const ul32 insn[] = {
      0x18000050, // ldr  w16, addr
      0x14000000, // b    stubHelperHeader
      0x00000000, // addr: .long <idx>
    };

    static_assert(sizeof(insn) == E::stub_helper_size);

    memcpy(buf, insn, sizeof(insn));
    buf[1] |= bits((start - buf - 1) * 4, 27, 2);
    buf[2] = ctx.stubs.bind_offsets[i];
    buf += 3;
  }
}

template <>
void ObjcStubsSection<E>::copy_buf(Context<E> &ctx) {
  if (this->hdr.size == 0)
    return;

  static const ul32 insn[] = {
    0x90000001, // adrp  x1, @selector("foo")@PAGE
    0xf9400021, // ldr   x1, [x1, @selector("foo")@PAGEOFF]
    0x90000010, // adrp  x16, _objc_msgSend@GOTPAGE
    0xf9400210, // ldr   x16, [x16, _objc_msgSend@GOTPAGEOFF]
    0xd61f0200, // br    x16
    0xd4200020, // brk   #0x1
    0xd4200020, // brk   #0x1
    0xd4200020, // brk   #0x1
  };
  static_assert(sizeof(insn) == ENTRY_SIZE);

  u64 msgsend_got_addr = get_symbol(ctx, "_objc_msgSend")->get_got_addr(ctx);

  for (i64 i = 0; i < methnames.size(); i++) {
    ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset + ENTRY_SIZE * i);
    u64 sel_addr = selrefs[i]->get_addr(ctx);
    u64 ent_addr = this->hdr.addr + ENTRY_SIZE * i;

    memcpy(buf, insn, sizeof(insn));
    buf[0] |= page_offset(sel_addr, ent_addr);
    buf[1] |= bits(sel_addr, 11, 3) << 10;
    buf[2] |= page_offset(msgsend_got_addr, ent_addr + 8);
    buf[3] |= bits(msgsend_got_addr, 11, 3) << 10;
  }
}

template <>
std::vector<Relocation<E>>
read_relocations(Context<E> &ctx, ObjectFile<E> &file, const MachSection &hdr) {
  std::vector<Relocation<E>> vec;
  vec.reserve(hdr.nreloc);

  MachRel *rels = (MachRel *)(file.mf->data + hdr.reloff);

  for (i64 i = 0; i < hdr.nreloc; i++) {
    i64 addend = 0;

    switch (rels[i].type) {
    case ARM64_RELOC_UNSIGNED:
    case ARM64_RELOC_SUBTRACTOR:
      switch (rels[i].p2size) {
      case 2:
        addend = *(il32 *)((u8 *)file.mf->data + hdr.offset + rels[i].offset);
        break;
      case 3:
        addend = *(il64 *)((u8 *)file.mf->data + hdr.offset + rels[i].offset);
        break;
      default:
        unreachable();
      }
      break;
    case ARM64_RELOC_ADDEND:
      addend = rels[i++].idx;
      break;
    }

    MachRel &r = rels[i];
    vec.push_back({r.offset, (u8)r.type, (u8)r.p2size});

    Relocation<E> &rel = vec.back();

    if (i > 0 && rels[i - 1].type == ARM64_RELOC_SUBTRACTOR)
      rel.is_subtracted = true;

    if (!rel.is_subtracted && rels[i].type != ARM64_RELOC_SUBTRACTOR)
      rel.is_pcrel = r.is_pcrel;

    if (r.is_extern) {
      rel.sym = file.syms[r.idx];
      rel.addend = addend;
      continue;
    }

    u64 addr = r.is_pcrel ? (hdr.addr + r.offset + addend) : addend;
    Subsection<E> *target = file.find_subsection(ctx, r.idx - 1, addr);
    if (!target)
      Fatal(ctx) << file << ": bad relocation: " << r.offset;

    rel.subsec = target;
    rel.addend = addr - target->input_addr;
  }

  return vec;
}

template <>
void Subsection<E>::scan_relocations(Context<E> &ctx) {
  for (Relocation<E> &r : get_rels()) {
    Symbol<E> *sym = r.sym;
    if (!sym)
      continue;

    if (sym->is_imported && sym->file->is_dylib)
      ((DylibFile<E> *)sym->file)->is_alive = true;

    switch (r.type) {
    case ARM64_RELOC_UNSIGNED:
      if (sym->is_imported) {
        if (r.p2size != 3) {
          Error(ctx) << this->isec << ": " << r << " relocation at offset 0x"
                     << std::hex << r.offset << " against symbol `"
                     << *sym << "' can not be used";
        }
        r.needs_dynrel = true;
      }
      break;
    case ARM64_RELOC_GOT_LOAD_PAGE21:
    case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
    case ARM64_RELOC_POINTER_TO_GOT:
      sym->flags |= NEEDS_GOT;
      break;
    case ARM64_RELOC_TLVP_LOAD_PAGE21:
    case ARM64_RELOC_TLVP_LOAD_PAGEOFF12:
      sym->flags |= NEEDS_THREAD_PTR;
      break;
    }

    if (sym->is_imported)
      sym->flags |= NEEDS_STUB;
  }
}

template <>
void Subsection<E>::apply_reloc(Context<E> &ctx, u8 *buf) {
  std::span<Relocation<E>> rels = get_rels();

  for (i64 i = 0; i < rels.size(); i++) {
    Relocation<E> &r = rels[i];
    u8 *loc = buf + r.offset;
    i64 val = r.addend;
    u64 pc = get_addr(ctx) + r.offset;

    if (r.sym && !r.sym->file) {
      Error(ctx) << "undefined symbol: " << isec.file << ": " << *r.sym;
      continue;
    }

    // Compute a relocated value.
    switch (r.type) {
    case ARM64_RELOC_UNSIGNED:
    case ARM64_RELOC_BRANCH26:
    case ARM64_RELOC_PAGE21:
    case ARM64_RELOC_PAGEOFF12:
      val += r.sym ? r.sym->get_addr(ctx) : r.subsec->get_addr(ctx);
      break;
    case ARM64_RELOC_SUBTRACTOR: {
      Relocation<E> s = rels[++i];
      assert(s.type == ARM64_RELOC_UNSIGNED);
      u64 val1 = r.sym ? r.sym->get_addr(ctx) : r.subsec->get_addr(ctx);
      u64 val2 = s.sym ? s.sym->get_addr(ctx) : s.subsec->get_addr(ctx);
      val += val2 - val1;
      break;
    }
    case ARM64_RELOC_GOT_LOAD_PAGE21:
    case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
    case ARM64_RELOC_POINTER_TO_GOT:
      val += r.sym->get_got_addr(ctx);
      break;
    case ARM64_RELOC_TLVP_LOAD_PAGE21:
    case ARM64_RELOC_TLVP_LOAD_PAGEOFF12:
      val += r.sym->get_tlv_addr(ctx);
      break;
    default:
      Fatal(ctx) << isec << ": unknown reloc: " << (int)r.type;
    }

    // An address of a thread-local variable is computed as an offset
    // to the beginning of the first thread-local section.
    if (isec.hdr.type == S_THREAD_LOCAL_VARIABLES)
      val -= ctx.tls_begin;

    // Write a computed value to the output buffer.
    switch (r.type) {
    case ARM64_RELOC_UNSIGNED:
    case ARM64_RELOC_SUBTRACTOR:
    case ARM64_RELOC_POINTER_TO_GOT:
      if (r.is_pcrel)
        val -= pc;

      if (r.p2size == 2)
        *(ul32 *)loc = val;
      else if (r.p2size == 3)
        *(ul64 *)loc = val;
      else
        unreachable();
      break;
    case ARM64_RELOC_BRANCH26: {
      assert(r.is_pcrel);
      val -= pc;

      i64 lo = -(1 << 27);
      i64 hi = 1 << 27;

      if (val < lo || hi <= val) {
        val = isec.osec.thunks[r.thunk_idx]->get_addr(r.thunk_sym_idx) - pc;
        assert(lo <= val && val < hi);
      }

      *(ul32 *)loc |= bits(val, 27, 2);
      break;
    }
    case ARM64_RELOC_PAGE21:
    case ARM64_RELOC_GOT_LOAD_PAGE21:
    case ARM64_RELOC_TLVP_LOAD_PAGE21:
      assert(r.is_pcrel);
      *(ul32 *)loc |= page_offset(val, pc);
      break;
    case ARM64_RELOC_PAGEOFF12:
    case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
    case ARM64_RELOC_TLVP_LOAD_PAGEOFF12: {
      assert(!r.is_pcrel);
      u32 insn = *(ul32 *)loc;
      i64 scale = 0;
      if ((insn & 0x3b000000) == 0x39000000) {
        scale = bits(insn, 31, 30);
        if (scale == 0 && (insn & 0x04800000) == 0x04800000)
          scale = 4;
      }
      *(ul32 *)loc |= bits(val, 11, scale) << 10;
      break;
    }
    default:
      Fatal(ctx) << isec << ": unknown reloc: " << (int)r.type;
    }
  }
}

void RangeExtensionThunk<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + output_section.hdr.offset + offset;

  static const ul32 data[] = {
    0x90000010, // adrp x16, 0   # R_AARCH64_ADR_PREL_PG_HI21
    0x91000210, // add  x16, x16 # R_AARCH64_ADD_ABS_LO12_NC
    0xd61f0200, // br   x16
  };

  static_assert(ENTRY_SIZE == sizeof(data));

  for (i64 i = 0; i < symbols.size(); i++) {
    u64 addr = symbols[i]->get_addr(ctx);
    u64 pc = output_section.hdr.addr + offset + i * ENTRY_SIZE;

    u8 *loc = buf + i * ENTRY_SIZE;
    memcpy(loc , data, sizeof(data));
    *(ul32 *)loc |= page_offset(addr, pc);
    *(ul32 *)(loc + 4) |= bits(addr, 11, 0) << 10;
  }
}

} // namespace mold::macho
