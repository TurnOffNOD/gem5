/*
 * Copyright (c) 2003 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string>

#include "cpu/base_cpu.hh"
#include "cpu/exec_context.hh"

#ifdef FULL_SYSTEM
#include "sim/system.hh"
#else
#include "sim/process.hh"
#endif

using namespace std;

// constructor
#ifdef FULL_SYSTEM
ExecContext::ExecContext(BaseCPU *_cpu, int _thread_num, System *_sys,
                         AlphaITB *_itb, AlphaDTB *_dtb,
                         FunctionalMemory *_mem)
    : _status(ExecContext::Unallocated),
      kernelStats(this, _cpu), cpu(_cpu), thread_num(_thread_num),
      cpu_id(-1), mem(_mem), itb(_itb), dtb(_dtb), system(_sys),
      memCtrl(_sys->memCtrl), physmem(_sys->physmem),
      swCtx(NULL), func_exe_inst(0), storeCondFailures(0)
{
    memset(&regs, 0, sizeof(RegFile));
}
#else
ExecContext::ExecContext(BaseCPU *_cpu, int _thread_num,
                         Process *_process, int _asid)
    : _status(ExecContext::Unallocated),
      cpu(_cpu), thread_num(_thread_num), cpu_id(-1),
      process(_process), mem(process->getMemory()), asid(_asid),
      func_exe_inst(0), storeCondFailures(0)
{
    memset(&regs, 0, sizeof(RegFile));
}

ExecContext::ExecContext(BaseCPU *_cpu, int _thread_num,
                         FunctionalMemory *_mem, int _asid)
    : cpu(_cpu), thread_num(_thread_num), process(0), mem(_mem), asid(_asid),
      func_exe_inst(0), storeCondFailures(0)
{
    memset(&regs, 0, sizeof(RegFile));
}
#endif


void
ExecContext::takeOverFrom(ExecContext *oldContext)
{
    // some things should already be set up
    assert(mem == oldContext->mem);
#ifdef FULL_SYSTEM
    assert(system == oldContext->system);
#else
    assert(process == oldContext->process);
#endif

    // copy over functional state
    _status = oldContext->_status;
#ifdef FULL_SYSTEM
    kernelStats = oldContext->kernelStats;
#endif
    regs = oldContext->regs;
    cpu_id = oldContext->cpu_id;
    func_exe_inst = oldContext->func_exe_inst;

    storeCondFailures = 0;

    oldContext->_status = ExecContext::Unallocated;
}


void
ExecContext::serialize(ostream &os)
{
    SERIALIZE_ENUM(_status);
    regs.serialize(os);
    // thread_num and cpu_id are deterministic from the config
    SERIALIZE_SCALAR(func_exe_inst);
    SERIALIZE_SCALAR(inst);

#ifdef FULL_SYSTEM
    bool ctx = false;
    if (swCtx) {
        ctx = true;
        SERIALIZE_SCALAR(ctx);
        SERIALIZE_SCALAR(swCtx->calls);
        std::stack<fnCall *> *stack = &(swCtx->callStack);
        fnCall *top;
        int size = stack->size();
        SERIALIZE_SCALAR(size);

        for (int j=0; j<size; ++j) {
            top = stack->top();
            paramOut(os, csprintf("stackpos[%d]",j), top->name);
            delete top;
            stack->pop();
        }
    } else {
        SERIALIZE_SCALAR(ctx);
    }
    if (system->bin) {
        Statistics::MainBin *cur = Statistics::MainBin::curBin();
        string bin_name = cur->name();
        SERIALIZE_SCALAR(bin_name);
    }
#endif //FULL_SYSTEM
}


void
ExecContext::unserialize(Checkpoint *cp, const std::string &section)
{
    UNSERIALIZE_ENUM(_status);
    regs.unserialize(cp, section);
    // thread_num and cpu_id are deterministic from the config
    UNSERIALIZE_SCALAR(func_exe_inst);
    UNSERIALIZE_SCALAR(inst);

#ifdef FULL_SYSTEM
    bool ctx;
    UNSERIALIZE_SCALAR(ctx);
    if (ctx) {
        swCtx = new SWContext;
        UNSERIALIZE_SCALAR(swCtx->calls);
        int size;
        UNSERIALIZE_SCALAR(size);

        vector<fnCall *> calls;
        fnCall *call;
        for (int i=0; i<size; ++i) {
            call = new fnCall;
            paramIn(cp, section, csprintf("stackpos[%d]",i), call->name);
            call->myBin = system->getBin(call->name);
            calls.push_back(call);
        }

        for (int i=size-1; i>=0; --i) {
            swCtx->callStack.push(calls[i]);
        }

    }

    if (system->bin) {
        string bin_name;
        UNSERIALIZE_SCALAR(bin_name);
        system->getBin(bin_name)->activate();
    }
#endif //FULL_SYSTEM
}


void
ExecContext::activate(int delay)
{
    if (status() == Active)
        return;

    _status = Active;
    cpu->activateContext(thread_num, delay);
}

void
ExecContext::suspend()
{
    if (status() == Suspended)
        return;

#ifdef FULL_SYSTEM
    // Don't change the status from active if there are pending interrupts
    if (cpu->check_interrupts()) {
        assert(status() == Active);
        return;
    }
#endif

    _status = Suspended;
    cpu->suspendContext(thread_num);
}

void
ExecContext::deallocate()
{
    if (status() == Unallocated)
        return;

    _status = Unallocated;
    cpu->deallocateContext(thread_num);
}

void
ExecContext::halt()
{
    if (status() == Halted)
        return;

    _status = Halted;
    cpu->haltContext(thread_num);
}


void
ExecContext::regStats(const string &name)
{
#ifdef FULL_SYSTEM
    kernelStats.regStats(name + ".kern");
#endif
}

void
ExecContext::trap(Fault fault)
{
    //TheISA::trap(fault);    //One possible way to do it...

    /** @todo: Going to hack it for now.  Do a true fixup later. */
#ifdef FULL_SYSTEM
    ev5_trap(fault);
#else
    fatal("fault (%d) detected @ PC 0x%08p", fault, readPC());
#endif
}
