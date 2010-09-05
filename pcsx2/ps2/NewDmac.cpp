/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


#include "PrecompiledHeader.h"
#include "NewDmac.h"
#include "HwInternal.h"

#include "NewDmac_Tables.inl"
#include "NewDmac_ChainMode.inl"

static uint round_robin = 1;

using namespace EE_DMAC;

ControllerMetrics dmac_metrics;

// When the DMAC is operating in strict emulation mode, channels only receive arbitration rights
// when their dma_request flag is set.  This is an internal DMAC value that appears to have no
// equivalent hardware register on the PS2, and it is flagged TRUE under the following conditions:
//  * The channel has received a STR=1 command.
//  * The channel's accompanying FIFO has drained, and the channel has STR=1.
//    (GIF FIFO is 16 QWC, VIF FIFOs appear to be 8 QWC).
//
// [TODO] Fully implement strict DMA timing mode.
//
static bool dma_request[NumChannels];

bool EE_DMAC::ChannelState::TestArbitration()
{
	if (!chcr.STR) return false;

	if (UseStrictDmaTiming && !dma_request[round_robin])
	{
		// Strict DMA Timings!
		// In strict mode each DMA channel has built-in timers that monitor their FIFO drain
		// rates (actual DMA channel FIFOs are not emulated, only their burst copy size is
		// enforced, which is typically 16 QWC for GIF and 8 QWC for VIF and SIF).
		// When a peripheral deems its FIFO as full or empty (depending on src/drain flag),
		// the DMA flags a "DREQ" (DMA Request), and at that point it is ready for selection.

		return false;
	}

	// Metrics: It's easier to just assume it gets skipped, and decrement the counter
	// later if, in fact, it isn't skipped. ;)
	if (IsDevBuild) ++dmac_metrics.channel[round_robin].skipped_arbitrations;

	if (dmacRegs.pcr.PCE && !(dmacRegs.pcr.CDE & (1<<round_robin)))
	{
		DMA_LOG("\t%s bypassed due to PCE/CDE%d condition", info.NameA, round_robin);
		return false;
	}

	if (DrainStallActive())
	{
		// this channel has drain stalling enabled.  If the stall condition is already met
		// then we need to skip it by and try another channel.

		// Drain Stalling Rules:
		//  * We can copy data up to STADR (exclusive).
		//  * Arbitration should not granted until at least 8 QWC is available for copy.
		//  * If the source DMA doesn't round out at 8 QWC, then the drain channel will
		//    refuse to transfer a partial QWC and will stall indefinitely (until STR is
		//    manually cleared).
		//
		// The last two "features" are difficult to emulate, and are currently on the
		// [TODO] list.
					
		uint stallAt = dmacRegs.stadr.ADDR;
		uint endAt = madr.ADDR + 8*16;

		if (!madr.SPR)
		{
			if (endAt > stallAt)
			{
				DMAC_LOG("\t%s bypassed due to DRAIN STALL condition (D%d_MADR=0x%08x, STADR=0x%08x",
					info.NameA, madr.ADDR, dmacRegs.stadr.ADDR);

				return false;
			}
		}
		else if (stallAt < Ps2MemSize::Scratch)
		{
			// Assumptions:
			// SPR bit transfers most likely perform automatic memory wrapping/masking on MADR
			// and likely do not automatically mask/wrap STADR (both of these assertions
			// need proper test app confirmations!! -- air)

			if ((madr.ADDR < stallAt) && (endAt > stallAt))
			{
				DMAC_LOG("\t%s bypassed due to DRAIN STALL condition (D%d_MADR=%s, STADR=0x%08x)",
					info.NameA, madr.ToUTF8().data(), dmacRegs.stadr.ADDR);

				return false;
			}
			else
			{
				endAt &= (Ps2MemSize::Scratch-1);
				if ((madr.ADDR >= stallAt) && (endAt > stallAt))
				{
					DMAC_LOG("\t%s bypassed due to DRAIN STALL condition (D%d_MADR=%s, STADR=0x%08x) [SPR memory wrap!]",
						info.NameA, madr.ToUTF8().data(), dmacRegs.stadr.ADDR);

					return false;
				}
			}
		}
	}

	if (UseMFIFOHack && (Id == ChanId_fromSPR) && (dmacRegs.ctrl.MFD != NO_MFD) && (dmacRegs.ctrl.MFD != MFD_RESERVED))
	{
		// When the MFIFO hack is enabled, we ignore fromSPR's side of MFIFO.  VIF1
		// and GIF will drain directly from fromSPR when arbitration is passed to them.

		DMAC_LOG("fromSPR bypassed due to MFIFO/Hack condition.");
		return false;
	}

	if (IsDevBuild) --dmac_metrics.channel[Id].skipped_arbitrations;

	if (UseStrictDmaTiming) dma_request[Id] = false;

	return true;
}

// Returns the index of the next DMA channel granted bus rights.
static ChannelId ArbitrateBusRight()
{
	ControllerRegisters& dmacReg = (ControllerRegisters&)psHu8(DMAC_CTRL);

	//  * VIF0 has top priority.
	//  * SIF2 has secondary priority.
	//  * All other channels are managed in cyclic arbitration (round robin).

	wxString ActiveDmaMsg;

	// VIF0 is the highest of the high priorities!!
	const tDMA_CHCR& vif0chcr = ChannelInfo[ChanId_VIF0].CHCR();
	if (vif0chcr.STR)
	{
		if (!dmacRegs.pcr.PCE || (dmacRegs.pcr.CDE & 2)) return ChanId_VIF0;
		DMA_LOG("\tVIF0 bypassed due to PCE/CDE0 condition.");
		if (IsDevBuild) ++dmac_metrics.channel[ChanId_VIF0].skipped_arbitrations;
	}

	// SIF2 is next!!
	const tDMA_CHCR& sif2chcr = ChannelInfo[ChanId_SIF2].CHCR();
	if (sif2chcr.STR)
	{
		if (!dmacRegs.pcr.PCE || (dmacRegs.pcr.CDE & 2)) return ChanId_SIF2;
		DMA_LOG("\tSIF2 bypassed due to PCE/CDE0 condition.");
		if (IsDevBuild) ++dmac_metrics.channel[ChanId_SIF2].skipped_arbitrations;
	}

	// Everything else is handled round-robin style!
	for (uint lopi=0; lopi<NumChannels; ++lopi)
	{
		++round_robin;
		if (round_robin >= NumChannels) round_robin = 1;

		if (ChannelState( (ChannelId)round_robin ).TestArbitration())
			return (ChannelId)round_robin;
	}

	return ChanId_None;
}

void EE_DMAC::ChannelState::TransferInterleaveData()
{
	tDMA_SADR& sadr = info.SADR();

	// Interleave Data Transfer notes:
	//  * Interleave is allowed on toSPR and fromSPR only.
	//
	//  * SPR can transfer to itself.  The SPR bit is supposed to be ineffective, however
	//    directly addressing the SPR via 0x70000000 appears to work (but without applied
	//    wrapping logic).

	// Interleave should only be valid for toSPR and fromSPR DMAs only.  Fortunately
	// supporting them is trivial, so although I'm asserting on debug builds, all other
	// builds actually perform the interleaved memcpy to/from SPR ram (this just in case
	// the EE actually supports it in spite of being indicated otherwise).
	pxAssertMsg( info.isSprChannel, "DMAC: Interleave mode specified on Scratchpad channel!" );

	// Interleave should never be used in conjunction with MFIFO.  Most likely the Real
	// DMAC ignores MFIFO settings in this case, and performs a normal SPR<->Memory xfer.
	// (though its also possible the DMAC has an error starting the transfer or some such)
	if (IsDebugBuild)
		pxAssert( dmacRegs.ctrl.MFD <= MFD_RESERVED );
	else
		DevCon.Warning("(DMAC) MFIFO enabled during interleaved transfer (ignored!)");

	DMAC_LOG("\tTQWC=%u, SQWC=%u", dmacRegs.sqwc.TQWC, dmacRegs.sqwc.SQWC );
	pxAssumeDev( (creg.qwc.QWC % dmacRegs.sqwc.TQWC) == 0, "(DMAC INTERLEAVE) QWC is not evenly divisible by TQWC!" );

	uint tqwc = dmacRegs.sqwc.TQWC;
	if (!pxAssert(tqwc!=0))
	{
		// The old PCSX2 DMA code treats a zero-length TQWC as "copy a single row" --
		// TQWC is assumed to be the channel's QWC, and SQWC is applied normally afterward.
		// There doesn't appear to be any indication of this behavior, and the old DMA
		// code does not cite any specific game(s) this fixes.  Hmm!  --air

		tqwc = creg.qwc.QWC;
	}

	if (IsDevBuild && UseDmaBurstHack)
	{
		// Sanity Check:  The burst hack assumes that the Memory side of the transfer
		// will always be within the same mappable page of ram, and that it won't cross
		// some oddball boundary or spill over into unmapped ram.
		
		uint add = (tqwc + dmacRegs.sqwc.SQWC);

		tDMAC_ADDR endaddr = madr;
		endaddr.ADDR += add * 16;

		u128* startmem	= DMAC_GetHostPtr(madr, false);
		u128* endmem	= DMAC_TryGetHostPtr(endaddr, false);
		pxAssertDev( (endmem != NULL) && ((startmem+add) == endmem),
			"(DMAC) Physical memory cross-boundary violation detected on SPR INTERLEAVE transfer!"
		);
	}

	// The following interleave code is optimized for burst hack transfers, which
	// transfer all interleaved data at once; which is why we set up some pre-loop
	// variables and then flush them back to DMAC registers when the transfer is complete.

	uint curqwc = creg.qwc.QWC;
	uint addrtmp = sadr.ADDR / 16;

	if (GetDir() == Dir_Source)
	{
		// fromSPR -> Xfer from SPR to memory.

		u128* writeTo = DMAC_GetHostPtr(madr, true);

		do {

			MemCopy_WrappedSrc(
				(u128*)eeMem->Scratch, addrtmp,
				Ps2MemSize::Scratch/16, writeTo, tqwc
			);
			writeTo	+= tqwc + dmacRegs.sqwc.SQWC;
			curqwc	-= tqwc;
			addrtmp	+= dmacRegs.sqwc.SQWC;
			addrtmp	&= (Ps2MemSize::Scratch / 16) - 1;
		} while(UseDmaBurstHack && curqwc);

		if(dmacRegs.ctrl.STS == STS_fromSPR)
		{
			DMAC_LOG("\tUpdated STADR=%s (prev=%s)", madr.ToUTF8(false), dmacRegs.stadr.ToUTF8(false));
			dmacRegs.stadr = madr;
		}
	}
	else
	{
		// toSPR -> Drain from memory to SPR.
		// DMAC does not perform STADR checks in this direction.

		const u128* readFrom = DMAC_GetHostPtr(madr, false);

		do {
			MemCopy_WrappedDest(
				readFrom, (u128*)eeMem->Scratch,
				addrtmp, Ps2MemSize::Scratch/16, tqwc
			);
			readFrom+= tqwc + dmacRegs.sqwc.SQWC;
			curqwc	-= tqwc;
			addrtmp	+= dmacRegs.sqwc.SQWC;
			addrtmp	&= (Ps2MemSize::Scratch / 16) - 1;
		} while(UseDmaBurstHack && curqwc);
	}

	uint qwc_copied = creg.qwc.QWC - curqwc;
	sadr.ADDR = addrtmp * 16;
	madr.ADDR += qwc_copied;

	dmac_metrics.RecordXfer(Id, INTERLEAVE_MODE, qwc_copied);
}

void EE_DMAC::ChannelState::TransferNormalAndChainData()
{
	const ChannelInformation& fromSPR = ChannelInfo[ChanId_fromSPR];
	ChannelRegisters& fromSprReg = fromSPR.GetRegs();

	// Step 1 : Determine MADR and Copyable Length

	const DirectionMode dir = GetDir();
	uint qwc = creg.qwc.QWC;

	try
	{
		// CHAIN modes arbitrate per-packet regardless of slice or burst modes.
		// NORMAL modes arbitrate at 8 QWC slices.

		if (NORMAL_MODE == chcr.MOD)
		{
			if (!UseDmaBurstHack && IsSliced())
				qwc = std::min<uint>(creg.qwc.QWC, 8);
		}
		else // CHAIN_MODE
		{
			if (UseMFIFOHack && ((creg.chcr.TAG.ID == TAG_CNT) || (creg.chcr.TAG.ID == TAG_END)))
			{
				// MFIFOhack: We can't let the peripheral out-strip SPR.
				//  (REFx tags copy from sources other than our SPRdma, which is why we
				//   exclude them above).
				qwc = std::min<uint>(creg.qwc.QWC, fromSprReg.qwc.QWC);
			}
		}

		if (DrainStallActive())
		{
			// this channel has drain stalling enabled.  If the stall condition is already met
			// then we need to skip it by and try another channel.

			// Drain Stalling Rules:
			//  * We can copy data up to STADR (exclusive).
			//  * Arbitration is not granted until at least 8 QWC is available for copy.
			//  
			// Furthermore, there must be at *least* 8 QWCs available for transfer or the
			// DMA stalls.  Stall-control DMAs do not transfer partial QWCs at the edge of
			// STADR.  Translation: If the source DMA (the one writing to STADR) doesn't
			// transfer an even 8-qwc block, the drain DMA will actually deadlock until
			// the PS2 app manually writes 0 to STR!  (and this is correct!) --air

			uint stallAt = dmacRegs.stadr.ADDR;
			uint endAt = madr.ADDR + qwc*16;

			if (!madr.SPR)
			{
				if (endAt > stallAt)
				{
					qwc = (stallAt - madr.ADDR) / 16;
					DMAC_LOG("\tDRAIN STALL condition! (STADR=%s, newQWC=%u)", dmacRegs.stadr.ToUTF8(false), qwc);
				}
			}
			else if (stallAt < Ps2MemSize::Scratch)
			{
				// Assumptions:
				// SPR bit transfers most likely perform automatic memory wrapping/masking on MADR
				// and likely do not automatically mask/wrap STADR (both of these assertions
				// need proper test app confirmations!! -- air)

				if ((madr.ADDR < stallAt) && (endAt > stallAt))
				{
					qwc = (stallAt - madr.ADDR) / 16;
					DMAC_LOG("\tDRAIN STALL condition! (STADR=%s, newQWC=%u)", dmacRegs.stadr.ToUTF8(false), qwc);
				}
				else
				{
					endAt &= (Ps2MemSize::Scratch-1);
					if ((madr.ADDR >= stallAt) && (endAt > stallAt))
					{
						// Copy from madr->ScratchEnd and from ScratchStart->StallAt
						qwc = ((Ps2MemSize::Scratch - madr.ADDR) + stallAt) / 16;
						DMAC_LOG("\tDRAIN STALL condition (STADR=%s, newQWC=%u) [SPR memory wrap]", dmacRegs.stadr.ToUTF8(false), qwc);
					}
				}
			}
		}
		
		// The real hardware has undefined behavior for this, but PCSX2 supports it.
 		pxAssertMsg(creg.qwc.QWC < _1mb, "DMAC: QWC is over 1 meg!");

		// -----------------------------------
		// DO THAT MOVEMENT OF DATA.  NOOOOOW!		
		// -----------------------------------

		uint qwc_xfer = (dir == Dir_Source)
			? TransferSource(DMAC_GetHostPtr(madr,true), qwc)
			: TransferDrain(DMAC_GetHostPtr(madr,false), qwc);

		// Peripherals have the option to stall transfers on their end, usually due to
		// specific conditions that can arise, such as tag errors or IRQs.

		if (qwc_xfer != qwc)
		{
			DMAC_LOG( "\tPartial transfer %s peripheral (qwc=%u, xfer=%u)",
				(dir==Dir_Drain) ? "to" : "from",
				qwc, qwc_xfer
			);
		}

		creg.qwc.QWC -= qwc_xfer;
		if (0 == creg.qwc.QWC)
		{
			// NORMAL MODE: STR becomes 0 when transfer ends (qwc==0)
			// CHAIN MODE: hop to the next link in the chain!

			if (NORMAL_MODE == creg.chcr.MOD)
			{
				creg.chcr.STR = 0;
				dmacRegs.stat.CIS |= (1 << Id);
			}
			else // (CHAIN_MODE == creg.chcr.MOD)
			{
				// In order to process chains correctly, we must update TADR and MADR
				// in separate passes.  This mimics the real DMAC behavior, which itself
				// does not update/advance the TADR until after the current chain's transfer
				// has completed successfully.
				//
				// After TADR is established, the new TAG is loaded into the channel's CHCR,
				// and then the new MADR established.

				const DMAtag* tag;

				if (MFIFOActive())
				{
					pxAssumeDev(NORMAL_MODE == fromSprReg.chcr.MOD, "MFIFO error: fromSPR is not in NORMAL mode.");
					if (fromSprReg.chcr.STR)
						pxAssumeDev(fromSprReg.qwc.QWC >= 1, "(MFIFO) fromSPR is running but has a QWC of zero!?");

					// MFIFO Enabled on this channel.
					// Based on the UseMFIFOHack, there are two approaches here:
					//  1. Hack Enabled: Copy data directly to/from SPR and the MFD peripheral.
					//  2. Hack Disabled: Copy data to/from the ringbuffer specified by the
					//     RBOR and RBSR registers (requires lots of wrapped memcpys).

					MFIFO_SrcChainUpdateTADR();

					if (!chcr.STR)
						return;

					// Load next tag from TADR and store the upper 16 bits in CHCR.
					tag = (DMAtag*)DMAC_GetHostPtr(creg.tadr, false);
					chcr.tag16 = tag->Bits16to31();
					creg.qwc.QWC = tag->QWC;

					MFIFO_SrcChainUpdateMADR(*tag);
				}
				else
				{
					if (dir == Dir_Drain)
						SrcChainUpdateTADR();
					else
						DstChainUpdateTADR();

					if (!chcr.STR)
						return;

					// Load next tag from TADR and store the upper 16 bits in CHCR.
					tag = (DMAtag*)DMAC_GetHostPtr(creg.tadr, false);
					chcr.tag16 = tag->Bits16to31();
					creg.qwc.QWC = tag->QWC;

					if (dir == Dir_Drain)
						SrcChainUpdateMADR(*tag);
					else
						DstChainUpdateMADR();
				}

				if (chcr.TTE && (dir == Dir_Drain))
				{
					// Tag Transfer is enabled
					// -----------------------
					// DMAtag is transferred with the data.  Tag is 128 bits, but the lower 64
					// bits are masked to zero; which typically translates into harmless NOPs in
					// GIFtag and VIFcode lands.
 
					// * TTE's behavior regarding the lower 64 bits is currently a strong assumption,
					//   but can be confirmed easily using toSPR's Source Chain mode transfer.  Write
					//   dummy data to SPR memory, initiate a simple chain transfer with TTE=1, and
					//   read back the tracewarn.

					static __aligned16 u64 masked_tag[2] = {0,0};
					masked_tag[1] = tag->_u64;
					TransferDrain(masked_tag);
				}
			}
		}

	} catch( Exception::DmaRaiseIRQ& ex )
	{
		// Standard IRQ behavior for all channel errors is to stop the DMA (STR=0)
		// and set the CIS bit corresponding to the DMA channel.  Bus Errors set
		// the BEIS bit additionally.

		if (!ex.m_MFIFOstall)
		{
			chcr.STR = 0;
			dmacRegs.stat.CIS |= (1 << Id);
			dmacRegs.stat.BEIS = ex.m_BusError;
		}

		dmacRegs.stat.MEIS = ex.m_MFIFOstall;

		if (ex.m_Verbose)
		{
			Console.Warning(L"(DMAC) IRQ raised on %s(%u), cause=%s", info.NameW, Id, ex.m_Cause);
		}

		DMAC_LOG("IRQ Raised on %s(%u), cause=%s", info.NameA, Id, wxString(ex.m_Cause).ToUTF8().data());

		// arbitrate back to the EE for a while?
		//break;
	}

}

void EE_DMAC::ChannelState::TransferData()
{
	const char* const SrcDrainMsg = GetDir() ? "<-" : "->";

	DMAC_LOG("\tBus right granted to %s%s%s QWC=0x%4x MODE=%s",
		info.ToUTF8().data(), SrcDrainMsg, creg.madr.ToUTF8(),
		creg.qwc.QWC, chcr.ModeToUTF8()
	);

	// Interleave mode has special accelerated handling in burst mode, and a lot of
	// checks and assertions, so lets handle it from its own function to help maintain
	// programmer sanity.

	if (chcr.MOD == INTERLEAVE_MODE)
		TransferInterleaveData();
	else
		TransferNormalAndChainData();
}

void eeEvt_UpdateDmac()
{
	ControllerRegisters& dmacReg = (ControllerRegisters&)psHu8(DMAC_CTRL);

	DMA_LOG("(UpdateDMAC Event) D_CTRL=0x%08X", dmacRegs.ctrl._u32);

	if ((psHu32(DMAC_ENABLER) & (1<<16)) || dmacRegs.ctrl.DMAE)
	{
		// Do not reschedule the event.  The indirect HW reg handler will reschedule it when
		// the DMAC register(s) are written and the DMAC is fully re-enabled.
		DMA_LOG("DMAC disabled, no actions performed. (DMAE=%d, ENABLER=0x%08x", dmacRegs.ctrl.DMAE, psHu32(DMAC_ENABLER));
		return;
	}

	do {
		ChannelId chanId = ArbitrateBusRight();

		if (chanId == -1)
		{
			// Do not reschedule the event.  The indirect HW reg handler will reschedule it when
			// the STR bits are written and the DMAC is enabled.

			DMA_LOG("DMAC Arbitration complete.");
			break;
		}

		ChannelState cstate( chanId );
		cstate.TransferData();

	} while (UseDmaBurstHack);
	


	wxString CycStealMsg;
	if (dmacRegs.ctrl.RELE)
	{
		/* Cycle Stealing Enabled?

		This feature of the DMAC exists mainly for PS2 hardware performance reasons,
		though it is possible that a game could disable cycle stealing in a way to
		time the EE to dma activity:
		
		With cycle stealing disabled, any direct physical memory access by the EE 
		(anything not scratchpad and not a hardware register, basically) will cause
		the EE to stall until the entire DMA completes.  Thusly, a game *could* start
		a DMA, write or read physical memory, and then know with certainty that the
		very next EE instruction executed will be run only after the DMAC has completed
		its tasks.
		
		(unless the EE uses scratchpad for stackframe, this would mean the EE can't
		 respond to IRQs either until the transfer finishes -- hmm)
		*/
		
		CycStealMsg = wxsFormat(L"On/%d",8<<dmacRegs.ctrl.RCYC);
	}
	else
	{
		CycStealMsg = L"Off";
	}

	wxsFormat(L"[CycSteal:%s]", CycStealMsg);

	for( uint i=0; i<NumChannels; ++i )
	{
		
	}
}

// Tells the PCSX2 event scheduler to execute the DMAC handler (eeEvt_UpdateDmac)
void dmacScheduleEvent()
{
	// If the DMAC is completely disabled then no point in scheduling anything.
	if (!dmacRegs.ctrl.DMAE || (psHu32(DMAC_ENABLEW) & (1 << 16))) return;

	CPU_INT( DMAC_EVENT, 8 );
}

// Schedules a cpu-level exception in response to a DMA channel being completed.  Actual
// scheduling of the exception depends on the mask status of the corresponding channel irq.
void dmacChanInt( ChannelId id )
{
	pxAssume(eeEventTestIsActive);

	// IRQ is only raised if the bit status has changed.

	uint bit = 1<<id;
	bool curbit = !!(dmacRegs.stat.CIS & bit);

	dmacRegs.stat.CIS |= bit;
	
	if (dmacRegs.stat.CIM & bit)
		cpuSetNextEventDelta( 0 );
}

template< uint page >
__fi u32 dmacRead32( u32 mem )
{
	return psHu32(mem);
}

// Returns TRUE if the caller should do writeback of the register to eeHw; false if the
// register has no writeback, or if the writeback is handled internally.
template< uint page >
__fi bool dmacWrite32( u32 mem, mem32_t& value )
{
	// this bool is set true when important information is modified which affects the
	// operational status of the DMAC.
	
	iswitch(mem) {
	icase(DMAC_CTRL)
	{
		bool needs_event = false;

		const tDMAC_CTRL& newval = (tDMAC_CTRL&)value;
		const tDMAC_CTRL& oldval = (tDMAC_CTRL&)psHu32(mem);

		if (oldval.STS != newval.STS)
		{
			DMAC_LOG( "Stall control source [STS] changed from %s to %s",
				newval.STS ? ChannelInfo[StallSrcChan[oldval.STS]].NameA : "None",
				newval.STS ? ChannelInfo[StallSrcChan[newval.STS]].NameA : "None"
			);

			if (newval.STS != NO_STS)
			{
				// Enabling STS on a transfer-in-progress shouldn't affect anything
				// since the DMAC should already be re-raising events anyway, but
				// no harm in being safe:
				needs_event |= ChannelInfo[StallSrcChan[newval.STS]].CHCR().STR;
			}
		}

		if (oldval.STD != newval.STD)
		{
			DMAC_LOG( "Stall control drain [STD] changed from %s to %s",
				newval.STD ? ChannelInfo[StallDrainChan[oldval.STD]].NameA : "None",
				newval.STD ? ChannelInfo[StallDrainChan[newval.STD]].NameA : "None"
			);

			if (oldval.STD != NO_STD)
			{
				// Releasing the STD setting on an active channel might clear the stall
				// condition preventing it from completing, so raise an event in such cases:
				needs_event |= ChannelInfo[StallSrcChan[oldval.STD]].CHCR().STR;
			}
		}
		
		if (needs_event) dmacScheduleEvent();

		return false;
	}

	icase(DMAC_STAT)
	{
		tDMAC_STAT& curval = (tDMAC_STAT&)psHu32(mem);
		const tDMAC_STAT& newval = (tDMAC_STAT&)value;
		const tDMAC_STAT oldval = curval;

		// lower 16 bits: clear on 1
		// upper 16 bits: reverse on 1

		curval._u16[0] &= newval._u16[0];
		curval._u16[1] ^= newval._u16[1];

		// We only want to raise a cpu exception if one of the newly-unmasked bits (upper 16)
		// is also high in the lower side -- which technically means that we want to disregard
		// any bits that weren't changed.

		if( (curval._u16[1] & curval._u16[0] & newval._u16[1]) != 0 )
			cpuTestDMACInts();
		return false;
	}

	icase(DMAC_ENABLEW)
	{
		// ENABLEW has a single bit (bit 16) which can be set to 0 (enable DMAC) or 1 (disable DMAC).
		// We need to make sure the DMAC event chain is resumed when 1 is written.

		psHu32(DMAC_ENABLEW) = value;
		psHu32(DMAC_ENABLER) = value;

		dmacScheduleEvent();
		return false;
	}
	}

	if (page < 0x08 || page > 0x0d) return true;		// valid pages for individual DMA channels

	// Fall-through from above cases means that all we have left are the per-channel DMA registers;
	// such as CHCR, MADR, QWC, and others.  Only changes to STR matter from a virtual machine
	// point-of-view.  The rest of the registers are handled only for trace logging purposes.

	if ((mem & 0xf0) == 0)
	{
		// Since CHCR is being written, perform necessary STR tests so we know to schedule a DMA
		// or not.  (if STR is set to 1, schedule it.  If set to 0, do nothing).  Logging is
		// performed later, if enabled.

		tDMA_CHCR& newchcr = (tDMA_CHCR&)value;
		tDMA_CHCR& curchcr = (tDMA_CHCR&)psHu32(mem);

		if (newchcr.STR && !curchcr.STR)
			dmacScheduleEvent();
	}

	// First pass is to determine the channel being modified.   After that we can dispatch based
	// on the actual register of the channel being modified.  This allows us to reuse all the same
	// code for all 10 DMAs.

	if (!SysTraceActive(EE.DMAC)) return true;
	const ChannelInformation* info = NULL;

	#define dmaCase(num) icase(D##num##_CHCR) { info = &ChannelInfo[num]; }

	iswitch(mem & ~0x0ff) {
		dmaCase(0); dmaCase(1); dmaCase(2);
		dmaCase(3); dmaCase(4); dmaCase(5);
		dmaCase(6); dmaCase(7);
		dmaCase(8); dmaCase(9);
	}
	
	if (!info) return true;

	const tDMA_CHCR& curchcr = info->CHCR();
	FastFormatAscii tracewarn;

	switch(mem & 0x0ff)
	{
		case 0x00:		// CHCR
		{
			tDMA_CHCR& newchcr = (tDMA_CHCR&)value;

			if (!curchcr.STR)
			{
				if( newchcr.STR )
				{
					DMAC_LOG("%s DmaExec Received (STR set to 1).", info->NameA);
					
					// [TODO] Log all DMA settings at STR=1;
				}
			}
			else
			{
				// Writing STR while the channel is running is allowed so long as the entire DMAC
				// is completely disabled. Doing so otherwise produces undefined results (typically
				// that the transfer will stop at some unknown time in the future).

				if (info->CHCR().DIR != newchcr.DIR)
				{
					if ( !(psHu32(DMAC_ENABLER) & (1<<16)) )
						DevCon.WriteLn("%s stopped during active transfer!", info->NameA);
						
					DMAC_LOG("%s STR changed to %u (DMAC is suspended)", info->NameA, newchcr.DIR);
				}

				// The game is writing newchcr while the DMA channel is active (STR==1).  This is
				// typically an error if done *ever* (ENABLEW or not) and will product undefined
				// results on real hardware.
				static const char* tbl_LogicalTransferNames[] =
				{
					"NORMAL", "CHAIN", "INTERLEAVE", "UNDEFINED"
				};

				FastFormatAscii result;
				if (curchcr.MOD != newchcr.MOD)
					tracewarn.Write("\n\tCHCR.MOD changed to %s (oldval=%s)", info->NameA, tbl_LogicalTransferNames[newchcr.MOD], tbl_LogicalTransferNames[curchcr.MOD]);

				if (curchcr.ASP != newchcr.ASP)
					tracewarn.Write("\n\tCHCR.ASP changed to %u (oldval=%u)", info->NameA, newchcr.ASP, curchcr.ASP);
				
				if (curchcr.TTE != newchcr.TTE)
					tracewarn.Write("\n\tCHCR.TTE changed to %u (oldval=%u)", info->NameA, newchcr.TTE, curchcr.TTE);

				if (curchcr.TIE != newchcr.TIE)
					tracewarn.Write("\n\tCHCR.TIE changed to %u (oldval=%u)", info->NameA, newchcr.TIE, curchcr.TIE);

				if (curchcr.tag16 != newchcr.tag16)
					tracewarn.Write("\n\tCHCR.TAG changed to 0x%04x (oldval=0x04x)", info->NameA, newchcr.tag16, curchcr.tag16);
			}	
		}
		break;
		
		case 0x10:		// MADR
		{
			if (!info->CHCR().STR) break;

			const tDMAC_ADDR& madr = (tDMAC_ADDR&)value;

			if(madr != info->MADR())
				tracewarn.Write("\n\tMADR changed to %s (oldval=%s)", info->NameA, madr.ToUTF8(), info->MADR().ToUTF8());
		}
		break;
		
		case 0x20:		// QWC
		{
			if (!info->CHCR().STR) break;

			const tDMA_QWC& qwc = (tDMA_QWC&)value;

			if(qwc != info->QWC())
				tracewarn.Write("\n\tQWC changed to 0x%04x (oldval=0x%04x)", info->NameA, qwc, info->QWC());
		}
		break;

		// [TODO] Finish checks for other per-channel DMA registers.
	}

	if (!tracewarn.IsEmpty())
		DMAC_LOG("[Warning] %s modified mid-transfer: %s", tracewarn.c_str());

	return true;
}

template u32 dmacRead32<0x03>( u32 mem );

template bool dmacWrite32<0x00>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x01>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x02>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x03>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x04>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x05>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x06>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x07>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x08>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x09>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0a>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0b>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0c>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0d>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0e>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0f>( u32 mem, mem32_t& value );


uint __dmacall EE_DMAC::toVIF0	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::toGIF	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::toVIF1	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::toSIF1	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::toSIF2	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::toIPU	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::toSPR	(const u128* srcBase, uint srcSize, uint srcStartQwc, uint lenQwc) { return 0; }

uint __dmacall EE_DMAC::fromIPU	(u128* dest, uint destSize, uint destStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::fromSPR	(u128* dest, uint destSize, uint destStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::fromSIF0(u128* dest, uint destSize, uint destStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::fromSIF2(u128* dest, uint destSize, uint destStartQwc, uint lenQwc) { return 0; }
uint __dmacall EE_DMAC::fromVIF0(u128* dest, uint destSize, uint destStartQwc, uint lenQwc) { return 0; }


// --------------------------------------------------------------------------------------
//  EE_DMAC::ControllerMetrics  (implementations)
// --------------------------------------------------------------------------------------
u64 EE_DMAC::ControllerMetrics::GetQWC(LogicalTransferMode mode) const
{
	u64 result = 0;
	for( uint i=0; i<NumChannels; ++i )
		result += channel[i].qwc[mode];
	return result;
}

u64 EE_DMAC::ControllerMetrics::GetQWC() const
{
	u64 result = 0;
	for( uint i=0; i<NumChannels; ++i )
	for( uint m=0; m<4; ++m )
		result += channel[i].qwc[m];
	return result;
}

uint EE_DMAC::ControllerMetrics::GetTransferCount(LogicalTransferMode mode) const
{
	uint result = 0;
	for( uint i=0; i<NumChannels; ++i )
		result += channel[i].xfers[mode];
	return result;
}

uint EE_DMAC::ControllerMetrics::GetTransferCount() const
{
	uint result = 0;
	for( uint i=0; i<NumChannels; ++i )
		for( uint m=0; m<4; ++m )
			result += channel[i].xfers[m];
	return result;
}