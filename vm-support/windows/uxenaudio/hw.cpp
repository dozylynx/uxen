/*++

Copyright (c) 1997-2000  Microsoft Corporation All Rights Reserved

Module Name:

    hw.cpp

Abstract:

    Implementation of UXENAUDIO HW class. 
    UXENAUDIO HW has an array for storing mixer and volume settings
    for the topology.


--*/
/*
 * uXen changes:
 *
 * Copyright 2013-2015, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <uxenaudio.h>
#include "hw.h"

//=============================================================================
// CUXENAUDIOHW
//=============================================================================

//=============================================================================
#pragma code_seg("PAGE")
CUXENAUDIOHW::CUXENAUDIOHW()
: m_ulMux(0),
    m_bDevSpecific(FALSE),
    m_iDevSpecific(0),
    m_uiDevSpecific(0)
/*++

Routine Description:

    Constructor for UXENAUDIOHW. 

Arguments:

Return Value:

    void

--*/
{
    PAGED_CODE();
    
    MixerReset();
} // CUXENAUDIOHW
#pragma code_seg()


//=============================================================================
BOOL
CUXENAUDIOHW::bGetDevSpecific()
/*++

Routine Description:

  Gets the HW (!) Device Specific info

Arguments:

  N/A

Return Value:

  True or False (in this example).

--*/
{
    return m_bDevSpecific;
} // bGetDevSpecific

//=============================================================================
void
CUXENAUDIOHW::bSetDevSpecific
(
    IN  BOOL                bDevSpecific
)
/*++

Routine Description:

  Sets the HW (!) Device Specific info

Arguments:

  fDevSpecific - true or false for this example.

Return Value:

    void

--*/
{
    m_bDevSpecific = bDevSpecific;
} // bSetDevSpecific

//=============================================================================
INT
CUXENAUDIOHW::iGetDevSpecific()
/*++

Routine Description:

  Gets the HW (!) Device Specific info

Arguments:

  N/A

Return Value:

  int (in this example).

--*/
{
    return m_iDevSpecific;
} // iGetDevSpecific

//=============================================================================
void
CUXENAUDIOHW::iSetDevSpecific
(
    IN  INT                 iDevSpecific
)
/*++

Routine Description:

  Sets the HW (!) Device Specific info

Arguments:

  fDevSpecific - true or false for this example.

Return Value:

    void

--*/
{
    m_iDevSpecific = iDevSpecific;
} // iSetDevSpecific

//=============================================================================
UINT
CUXENAUDIOHW::uiGetDevSpecific()
/*++

Routine Description:

  Gets the HW (!) Device Specific info

Arguments:

  N/A

Return Value:

  UINT (in this example).

--*/
{
    return m_uiDevSpecific;
} // uiGetDevSpecific

//=============================================================================
void
CUXENAUDIOHW::uiSetDevSpecific
(
    IN  UINT                uiDevSpecific
)
/*++

Routine Description:

  Sets the HW (!) Device Specific info

Arguments:

  uiDevSpecific - int for this example.

Return Value:

    void

--*/
{
    m_uiDevSpecific = uiDevSpecific;
} // uiSetDevSpecific


//=============================================================================
BOOL
CUXENAUDIOHW::GetMixerMute
(
    IN  ULONG                   ulNode
)
/*++

Routine Description:

  Gets the HW (!) mute levels for UXENAUDIO

Arguments:

  ulNode - topology node id

Return Value:

  mute setting

--*/
{
    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        return m_MuteControls[ulNode];
    }

    return 0;
} // GetMixerMute

//=============================================================================
ULONG                       
CUXENAUDIOHW::GetMixerMux()
/*++

Routine Description:

  Return the current mux selection

Arguments:

Return Value:

  ULONG

--*/
{
    return m_ulMux;
} // GetMixerMux

//=============================================================================
LONG
CUXENAUDIOHW::GetMixerVolume
(   
    IN  ULONG                   ulNode,
    IN  LONG                    lChannel
)
/*++

Routine Description:

  Gets the HW (!) volume for UXENAUDIO.

Arguments:

  ulNode - topology node id

  lChannel - which channel are we setting?

Return Value:

  LONG - volume level

--*/
{
    UNREFERENCED_PARAMETER(lChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        return m_VolumeControls[ulNode];
    }

    return 0;
} // GetMixerVolume

//=============================================================================
#pragma code_seg("PAGE")
void 
CUXENAUDIOHW::MixerReset()
/*++

Routine Description:

  Resets the mixer registers.

Arguments:

Return Value:

    void

--*/
{
    PAGED_CODE();
    
    RtlFillMemory(m_VolumeControls, sizeof(LONG) * MAX_TOPOLOGY_NODES, 0xFF);
    RtlFillMemory(m_MuteControls, sizeof(BOOL) * MAX_TOPOLOGY_NODES, TRUE);
    
    // BUGBUG change this depending on the topology
    m_ulMux = 2;
} // MixerReset
#pragma code_seg()

//=============================================================================
void
CUXENAUDIOHW::SetMixerMute
(
    IN  ULONG                   ulNode,
    IN  BOOL                    fMute
)
/*++

Routine Description:

  Sets the HW (!) mute levels for UXENAUDIO

Arguments:

  ulNode - topology node id

  fMute - mute flag

Return Value:

    void

--*/
{
    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        m_MuteControls[ulNode] = fMute;
    }
} // SetMixerMute

//=============================================================================
void                        
CUXENAUDIOHW::SetMixerMux
(
    IN  ULONG                   ulNode
)
/*++

Routine Description:

  Sets the HW (!) mux selection

Arguments:

  ulNode - topology node id

Return Value:

    void

--*/
{
    m_ulMux = ulNode;
} // SetMixMux

//=============================================================================
void  
CUXENAUDIOHW::SetMixerVolume
(   
    IN  ULONG                   ulNode,
    IN  LONG                    lChannel,
    IN  LONG                    lVolume
)
/*++

Routine Description:

  Sets the HW (!) volume for UXENAUDIO.

Arguments:

  ulNode - topology node id

  lChannel - which channel are we setting?

  lVolume - volume level

Return Value:

    void

--*/
{
    UNREFERENCED_PARAMETER(lChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        m_VolumeControls[ulNode] = lVolume;
    }
} // SetMixerVolume
