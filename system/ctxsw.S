/* ctxsw.S - ctxsw (for x86) */

		.text
		.globl	ctxsw

/*------------------------------------------------------------------------
 * ctxsw -  X86 context switch; the call is ctxsw(&old_sp, &new_sp)
 *------------------------------------------------------------------------
 */
ctxsw:
		pushl	%ebp		/* Push ebp onto stack		*/
		movl	%esp,%ebp	/* Record current SP in ebp	*/
		pushfl			/* Push flags onto the stack	*/
		pushal			/* Push general regs. on stack	*/

		/* Save old segment registers here, if multiple allowed */

		movl	8(%ebp),%eax	/* Get mem location in which to	*/
					/*   save the old process's SP	*/
		movl	%esp,(%eax)	/* Save old process's SP	*/
		movl	12(%ebp),%eax	/* Get location from which to	*/
					/*   restore new process's SP	*/

		/* The next instruction switches from the old process's	*/
		/*   stack to the new process's stack.			*/

		movl	(%eax),%esp	/* Pop up new process's SP	*/

		/* Restore new seg. registers here, if multiple allowed */
		
		/* Switch page directory for new process */
		/* Get new process ID from proctab */
		/* We need to get the page directory from the process table */
		/* For now, we'll do this in C code called from resched */
		/* Actually, we can access proctab here if we know the new pid */
		/* But it's easier to do it in resched.c */

		popal			/* Restore general registers	*/
		movl	4(%esp),%ebp	/* Pick up ebp before restoring	*/
					/*   interrupts			*/
		popfl			/* Restore interrupt mask	*/
		add	$4,%esp		/* Skip saved value of ebp	*/
		ret			/* Return to new process	*/
