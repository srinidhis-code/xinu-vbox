/* pagefault_handler_disp.S - pagefault_handler_disp (x86) */

/*------------------------------------------------------------------------
 * pagefault_handler_disp  -  Interrupt dispatcher for page faults
 *------------------------------------------------------------------------
 */

        .text
        .globl  pagefault_handler_disp      # Page fault interrupt dispatcher
        .extern pagefault_handler

pagefault_handler_disp:
        pushal                  # Save all general purpose registers
        cli                     # Disable further interrupts (belt & suspenders)

        call    pagefault_handler   # Call high-level C handler (no args)

        sti                     # Restore interrupt status
        popal                   # Restore registers
        add     $4, %esp        # Skip error code pushed by CPU
        iret                    # Return from interrupt
