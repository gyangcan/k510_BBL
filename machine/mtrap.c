#include "mtrap.h"
#include "mcall.h"
#include "htif.h"
#include "atomic.h"
#include "bits.h"
#include "vm.h"
#include "uart.h"
#include "uart16550.h"
#include "finisher.h"
#include "fdt.h"
#include "unprivileged_memory.h"
#include "disabled_hart_mask.h"
#include "trigger.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#define CCTL_L1D_IX_INVAL	16
#define CCTL_L1D_IX_WB		17
#define CCTL_L1D_IX_RTAG	19

typedef struct {
  volatile uint64_t remote_cache_flush_va;
  volatile uint64_t remote_cache_flush_pa;
} remote_dcache_op_data_t;

remote_dcache_op_data_t remote_dcache_op_data[2] = {{0,0}, {0,0}};

void ipi_dcache_wb(void)
{

  int hartid = read_csr(mhartid);

  int i=0;
  uint64_t tag;
  uint64_t pa = remote_dcache_op_data[hartid].remote_cache_flush_pa;

  uint64_t org_mcctlbeginaddr = read_csr(mcctlbeginaddr);

  for(i=0; i<4; i++)
  {
    write_csr(mcctlbeginaddr, (i<<13)|(pa & (0xffUL<<5)));
    write_csr(mcctlcommand, CCTL_L1D_IX_RTAG); //read tag
    tag=read_csr(mcctldata);
    
    if( (tag & (1UL<<63)) && ( (pa>>12) == ((tag & 0xffffffffUL)>>2) ) )
    {
      write_csr(mcctlbeginaddr, (i<<13)|(pa & (0xffUL<<5)));
      write_csr(mcctlcommand,  CCTL_L1D_IX_WB); //index wb/inval		
      break;
    }
  }

  write_csr(mcctlbeginaddr, org_mcctlbeginaddr);

}

void ipi_dcache_invalid(void)
{

  int hartid = read_csr(mhartid);

  int i=0;
  uint64_t tag;
  uint64_t pa = remote_dcache_op_data[hartid].remote_cache_flush_pa;

  uint64_t org_mcctlbeginaddr = read_csr(mcctlbeginaddr);

  for(i=0; i<4; i++)
  {
    write_csr(mcctlbeginaddr, (i<<13)|(pa & (0xffUL<<5)));
    write_csr(mcctlcommand, CCTL_L1D_IX_RTAG); //read tag
    tag=read_csr(mcctldata);
    
    if( (tag & (1UL<<63)) && ( (pa>>12) == ((tag & 0xffffffffUL)>>2) ) )
    {
      write_csr(mcctlbeginaddr, (i<<13)|(pa & (0xffUL<<5)));
      write_csr(mcctlcommand,  CCTL_L1D_IX_INVAL); //index wb/inval		
      break;
    }
  }

  write_csr(mcctlbeginaddr, org_mcctlbeginaddr);

}

void __attribute__((noreturn)) bad_trap(uintptr_t* regs, uintptr_t dummy, uintptr_t mepc)
{
  die("machine mode: unhandlable trap %d @ %p", read_csr(mcause), mepc);
}

static uintptr_t mcall_console_putchar(uint8_t ch)
{
  if (uart) {
    uart_putchar(ch);
  } else if (uart16550) {
    uart16550_putchar(ch);
  } else if (htif) {
    htif_console_putchar(ch);
  }
  return 0;
}

void putstring(const char* s)
{
  while (*s)
    mcall_console_putchar(*s++);
}

void vprintm(const char* s, va_list vl)
{
  char buf[256];
  vsnprintf(buf, sizeof buf, s, vl);
  putstring(buf);
}

void printm(const char* s, ...)
{
  va_list vl;

  va_start(vl, s);
  vprintm(s, vl);
  va_end(vl);
}

static void send_ipi(uintptr_t recipient, int event)
{
  if (((disabled_hart_mask >> recipient) & 1)) return;
  atomic_or(&OTHER_HLS(recipient)->mipi_pending, event);
  mb();
  plic_sw_pending(recipient);
}

static uintptr_t mcall_console_getchar()
{
  if (uart) {
    return uart_getchar();
  } else if (uart16550) {
    return uart16550_getchar();
  } else if (htif) {
    return htif_console_getchar();
  } else {
    return '\0';
  }
}

static uintptr_t mcall_clear_ipi()
{
  return clear_csr(mip, MIP_SSIP) & MIP_SSIP;
}

static uintptr_t mcall_shutdown()
{
  poweroff(0);
}

static uintptr_t mcall_set_timer(uint64_t when)
{
  *HLS()->timecmp = when;
  clear_csr(mip, MIP_STIP);
  set_csr(mie, MIP_MTIP);
  return 0;
}

static uintptr_t mcall_set_pfm()
{
  clear_csr(slip, MIP_SOVFIP);
  set_csr(mie, MIP_MOVFIP);
  return 0;
}

static uintptr_t mcall_read_powerbrake()
{
  return read_csr(mpft_ctl);
}

static uintptr_t mcall_write_powerbrake(int val)
{
  write_csr(mpft_ctl, val);
  return 0;
}

static uintptr_t mcall_set_trigger(long type, uintptr_t data, unsigned int m,
                                   unsigned int s, unsigned int u)
{
  int ret;

  switch (type)
  {
    case TRIGGER_TYPE_ICOUNT:
      ret = trigger_set_icount(data, m, s, u);
      break;
    case TRIGGER_TYPE_ITRIGGER:
      ret = trigger_set_itrigger(data, m, s, u);
      break;
    case TRIGGER_TYPE_ETRIGGER:
      ret = trigger_set_etrigger(data, m, s, u);
      break;
    default:
      ret = -1;
      break;
  }
  return ret;
}

static void send_ipi_many(uintptr_t* pmask, int event)
{
  _Static_assert(MAX_HARTS <= 8 * sizeof(*pmask), "# harts > uintptr_t bits");
  uintptr_t mask = hart_mask;
  if (pmask)
    mask &= load_uintptr_t(pmask, read_csr(mepc));

  // send IPIs to everyone
  for (uintptr_t i = 0, m = mask; m; i++, m >>= 1)
    if (m & 1)
      send_ipi(i, event);

  if (event == IPI_SOFT)
    return;

  // wait until all events have been handled.
  // prevent deadlock by consuming incoming IPIs.
  uint32_t incoming_ipi = 0;
  for (uintptr_t i = 0, m = mask; m; i++, m >>= 1)
    if (m & 1)
      while (plic_sw_get_pending(i)) {
        plic_sw_claim();
        if (HLS()->plic_sw.source_id) {
          incoming_ipi |= 1 << (HLS()->plic_sw.source_id);
          plic_sw_complete();
        }
      }

  // if we got an IPI, restore it; it will be taken after returning
  if (incoming_ipi) {
    *(HLS()->plic_sw.pending) = incoming_ipi;
    mb();
  }
}

static void send_ipi_dcache_flush(uintptr_t mask, int event, uint64_t va, uint64_t pa)
{
  remote_dcache_op_data_t* data = (mask == 0x2) ? &remote_dcache_op_data[1] : &remote_dcache_op_data[0];

  data->remote_cache_flush_va = va;
  data->remote_cache_flush_pa = pa;

//  printm("send_ipi_dcache_flush: 1 , %lx, %lx, %lx, %lx \r\n", mask, event, va, pa);
  // send IPIs to everyone
  for (uintptr_t i = 0, m = mask; m; i++, m >>= 1)
    if (m & 1)
      send_ipi(i, event);

//  printm("send_ipi_dcache_flush: 2 \r\n");
  if (event == IPI_SOFT)
    return;

//  printm("send_ipi_dcache_flush: 3 \r\n");
  // wait until all events have been handled.
  // prevent deadlock by consuming incoming IPIs.
  uint32_t incoming_ipi = 0;
  for (uintptr_t i = 0, m = mask; m; i++, m >>= 1)
    if (m & 1)
      while (plic_sw_get_pending(i)) {
        plic_sw_claim();
        if (HLS()->plic_sw.source_id) {
          incoming_ipi |= 1 << (HLS()->plic_sw.source_id);
          plic_sw_complete();
        }
      }

//  printm("send_ipi_dcache_flush: 4 \r\n");
  // if we got an IPI, restore it; it will be taken after returning
  if (incoming_ipi) {
    *(HLS()->plic_sw.pending) = incoming_ipi;
    mb();
  }
}

uintptr_t mcall_get_cycles(uintptr_t* mtime_val)
{
    *mtime_val = *mtime;
     return 0;
}

void mcall_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  write_csr(mepc, mepc + 4);

  uintptr_t n = regs[17], arg0 = regs[10], arg1 = regs[11], arg2 = regs[12], retval, ipi_type;

  switch (n)
  {
    case SBI_GET_CYCLES:
      retval = mcall_get_cycles((uintptr_t *)arg0);
         break;
    case SBI_CONSOLE_PUTCHAR:
      retval = mcall_console_putchar(arg0);
      break;
    case SBI_CONSOLE_GETCHAR:
      retval = mcall_console_getchar();
      break;
    case SBI_SEND_IPI:
      ipi_type = IPI_SOFT;
      goto send_ipi;
    case SBI_REMOTE_SFENCE_VMA:
    case SBI_REMOTE_SFENCE_VMA_ASID:
      ipi_type = IPI_SFENCE_VMA;
      goto send_ipi;
    case SBI_REMOTE_FENCE_I:
      ipi_type = IPI_FENCE_I;
      goto send_ipi;
    case SBI_REMOTE_DCACHE_WB:
      ipi_type = IPI_DCACHE_WB;
      goto send_ipi_dcache;
    case SBI_REMOTE_DCACHE_INVALID:
      ipi_type = IPI_DCACHE_INVALID;
send_ipi_dcache:
      send_ipi_dcache_flush(arg0, ipi_type, arg1, arg2);
      retval = 0;
      break;
send_ipi:
      send_ipi_many((uintptr_t*)arg0, ipi_type);
      retval = 0;
      break;
    case SBI_CLEAR_IPI:
      retval = mcall_clear_ipi();
      break;
    case SBI_SHUTDOWN:
      retval = mcall_shutdown();
      break;
    case SBI_SET_TIMER:
#if __riscv_xlen == 32
      retval = mcall_set_timer(arg0 + ((uint64_t)arg1 << 32));
#else
      retval = mcall_set_timer(arg0);
#endif
      break;
    case SBI_TRIGGER:
      retval = mcall_set_trigger(arg0, arg1, 0, 0, arg2);
      break;
    case SBI_SET_PFM:
      retval = mcall_set_pfm();
      break;
    case SBI_READ_POWERBRAKE:
      retval = mcall_read_powerbrake();
      break;
    case SBI_WRITE_POWERBRAKE:
      retval = mcall_write_powerbrake(arg0);
      break;
    default:
      retval = -ENOSYS;
      break;
  }
  regs[10] = retval;
}

void redirect_trap(uintptr_t epc, uintptr_t mstatus, uintptr_t badaddr)
{
  write_csr(sbadaddr, badaddr);
  write_csr(sepc, epc);
  write_csr(scause, read_csr(mcause));
  write_csr(mepc, read_csr(stvec));

  uintptr_t new_mstatus = mstatus & ~(MSTATUS_SPP | MSTATUS_SPIE | MSTATUS_SIE);
  uintptr_t mpp_s = MSTATUS_MPP & (MSTATUS_MPP >> 1);
  new_mstatus |= (mstatus * (MSTATUS_SPIE / MSTATUS_SIE)) & MSTATUS_SPIE;
  new_mstatus |= (mstatus / (mpp_s / MSTATUS_SPP)) & MSTATUS_SPP;
  new_mstatus |= mpp_s;
  write_csr(mstatus, new_mstatus);

  extern void __redirect_trap();
  return __redirect_trap();
}

void pmp_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  redirect_trap(mepc, read_csr(mstatus), read_csr(mbadaddr));
}

static void machine_page_fault(uintptr_t* regs, uintptr_t dummy, uintptr_t mepc)
{
  // MPRV=1 iff this trap occurred while emulating an instruction on behalf
  // of a lower privilege level. In that case, a2=epc and a3=mstatus.
  if (read_csr(mstatus) & MSTATUS_MPRV) {
    return redirect_trap(regs[12], regs[13], read_csr(mbadaddr));
  }
  bad_trap(regs, dummy, mepc);
}

void trap_from_machine_mode(uintptr_t* regs, uintptr_t dummy, uintptr_t mepc)
{
  printm("trap_from_machine_mode: mbadaddr=%x , mepc=%p, ints=%x, sepc=%p, mcause=%x\r\n",read_csr(mbadaddr), mepc, *(uint32_t *)read_csr(mepc), read_csr(sepc), read_csr(mcause));
  uintptr_t mcause = read_csr(mcause);

  switch (mcause)
  {
    case CAUSE_LOAD_PAGE_FAULT:
    case CAUSE_STORE_PAGE_FAULT:
    case CAUSE_FETCH_ACCESS:
    case CAUSE_LOAD_ACCESS:
    case CAUSE_STORE_ACCESS:
      return machine_page_fault(regs, dummy, mepc);
    default:
      bad_trap(regs, dummy, mepc);
  }
}

void poweroff(uint16_t code)
{
  printm("Power off\r\n");
  finisher_exit(code);
  if (htif) {
    htif_poweroff();
  } else {
    send_ipi_many(0, IPI_HALT);
    while (1) { asm volatile ("wfi\n"); }
  }
}
