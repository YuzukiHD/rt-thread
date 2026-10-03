/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __USB_PHY_SUN252I_H__
#define __USB_PHY_SUN252I_H__

#include <rtthread.h>
#include <drivers/ofw.h>

enum sun252i_usb_role
{
    SUN252I_USB_DEVICE = 0,
    SUN252I_USB_HOST,
};

struct sun252i_usb_phy;

/* the PHY a controller node names in its "phys" property */
struct sun252i_usb_phy *sun252i_usb_phy_get(struct rt_ofw_node *controller);

/* The PHY is shared by the device and the host controllers: a role is taken exclusively and
 * released before the other one starts. */
int sun252i_usb_phy_acquire(struct sun252i_usb_phy *phy, enum sun252i_usb_role role);
void sun252i_usb_phy_release(struct sun252i_usb_phy *phy, enum sun252i_usb_role role);

#endif
