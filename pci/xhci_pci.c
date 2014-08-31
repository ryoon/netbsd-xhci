/*	$NetBSD: xhci_pci.c,v 1.3 2014/03/29 19:28:25 christos Exp $	*/
/*	OpenBSD: xhci_pci.c,v 1.4 2014/07/12 17:38:51 yuo Exp	*/

/*
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: xhci_pci.c,v 1.3 2014/03/29 19:28:25 christos Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/proc.h>
#include <sys/queue.h>

#include <sys/bus.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usb_mem.h>

#include <dev/usb/xhcireg.h>
#include <dev/usb/xhcivar.h>

struct xhci_pci_quirk {
	pci_vendor_id_t		vendor;
	pci_product_id_t	product;
	int			quirks;
};

static const struct xhci_pci_quirk xhci_pci_quirks[] = {
	{ PCI_VENDOR_INTEL, PCI_PRODUCT_INTEL_CORE4G_M_XHCI,
	    XHCI_QUIRK_FORCE_INTR },
};

struct xhci_pci_softc {
	struct xhci_softc	sc_xhci;
	pci_chipset_tag_t	sc_pc;
	pcitag_t		sc_tag;
};

static int
xhci_pci_has_quirk(pci_vendor_id_t vendor, pci_product_id_t product)
{
	int i;

	for (i = 0; i < __arraycount(xhci_pci_quirks); i++)
		if (vendor == xhci_pci_quirks[i].vendor &&
		    product == xhci_pci_quirks[i].product)
			return xhci_pci_quirks[i].quirks;
	return 0;
}

static int
xhci_pci_match(device_t parent, cfdata_t match, void *aux)
{
	struct pci_attach_args *pa = (struct pci_attach_args *) aux;

	if (PCI_CLASS(pa->pa_class) == PCI_CLASS_SERIALBUS &&
	    PCI_SUBCLASS(pa->pa_class) == PCI_SUBCLASS_SERIALBUS_USB &&
	    PCI_INTERFACE(pa->pa_class) == PCI_INTERFACE_XHCI)
		return 1;

	return 0;
}

static int
xhci_pci_port_route(struct xhci_pci_softc *psc)
{
	struct xhci_softc * const sc = &psc->sc_xhci;

	pcireg_t val;

	/*
	 * Check USB3 Port Routing Mask register that indicates the ports
	 * can be changed from OS, and turn on by USB3 Port SS Enable register.
	 */
	val = pci_conf_read(psc->sc_pc, psc->sc_tag, PCI_XHCI_INTEL_USB3PRM);
	aprint_debug_dev(sc->sc_dev,
	    "USB3PRM / USB3.0 configurable ports: 0x%08x\n", val);

	pci_conf_write(psc->sc_pc, psc->sc_tag, PCI_XHCI_INTEL_USB3_PSSEN, val);
	val = pci_conf_read(psc->sc_pc, psc->sc_tag,PCI_XHCI_INTEL_USB3_PSSEN);
	aprint_debug_dev(sc->sc_dev,
	    "USB3_PSSEN / Enabled USB3.0 ports under xHCI: 0x%08x\n", val);

	/*
	 * Check USB2 Port Routing Mask register that indicates the USB2.0
	 * ports to be controlled by xHCI HC, and switch them to xHCI HC.
	 */
	val = pci_conf_read(psc->sc_pc, psc->sc_tag, PCI_XHCI_INTEL_USB2PRM);
	aprint_debug_dev(sc->sc_dev,
	    "XUSB2PRM / USB2.0 ports can switch from EHCI to xHCI:"
	    "0x%08x\n", val);
	pci_conf_write(psc->sc_pc, psc->sc_tag, PCI_XHCI_INTEL_XUSB2PR, val);
	val = pci_conf_read(psc->sc_pc, psc->sc_tag, PCI_XHCI_INTEL_XUSB2PR);
	aprint_debug_dev(sc->sc_dev,
	    "XUSB2PR / USB2.0 ports under xHCI: 0x%08x\n", val);

	return 0;
}

static void
xhci_pci_attach(device_t parent, device_t self, void *aux)
{
	struct xhci_pci_softc * const psc = device_private(self);
	struct xhci_softc * const sc = &psc->sc_xhci;
	struct pci_attach_args *const pa = (struct pci_attach_args *)aux;
	const pci_chipset_tag_t pc = pa->pa_pc;
	const pcitag_t tag = pa->pa_tag;
	char const *intrstr;
	pci_intr_handle_t ih;
	pcireg_t csr, memtype;
	int err;
	//const char *vendor;
	uint32_t hccparams;
	char intrbuf[PCI_INTRSTR_LEN];

	sc->sc_dev = self;
	sc->sc_bus.hci_private = sc;

	pci_aprint_devinfo(pa, "USB Controller");

	/* Check for quirks */
	sc->sc_xhci_quirks = xhci_pci_has_quirk(PCI_VENDOR(pa->pa_id),
						PCI_PRODUCT(pa->pa_id));

	/* check if memory space access is enabled */
	csr = pci_conf_read(pc, tag, PCI_COMMAND_STATUS_REG);
#ifdef DEBUG
	printf("csr: %08x\n", csr);
#endif
	if ((csr & PCI_COMMAND_MEM_ENABLE) == 0) {
		aprint_error_dev(self, "memory access is disabled\n");
		return;
	}

	/* map MMIO registers */
	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_CBMEM);
	switch (memtype) {
	case PCI_MAPREG_TYPE_MEM | PCI_MAPREG_MEM_TYPE_32BIT:
	case PCI_MAPREG_TYPE_MEM | PCI_MAPREG_MEM_TYPE_64BIT:
		if (pci_mapreg_map(pa, PCI_CBMEM, memtype, 0,
			   &sc->sc_iot, &sc->sc_ioh, NULL, &sc->sc_ios)) {
			sc->sc_ios = 0;
			aprint_error_dev(self, "can't map mem space\n");
			return;
		}
		break;
	default:
		aprint_error_dev(self, "BAR not 64 or 32-bit MMIO\n");
		return;
		break;
	}

	psc->sc_pc = pc;
	psc->sc_tag = tag;

	hccparams = bus_space_read_4(sc->sc_iot, sc->sc_ioh, 0x10);

	if (pci_dma64_available(pa) && ((hccparams&1)==1))
		sc->sc_bus.dmatag = pa->pa_dmat64;
	else
		sc->sc_bus.dmatag = pa->pa_dmat;

	/* Enable the device. */
	pci_conf_write(pc, tag, PCI_COMMAND_STATUS_REG,
		       csr | PCI_COMMAND_MASTER_ENABLE);

	/* Map and establish the interrupt. */
	if (pci_intr_map(pa, &ih)) {
		aprint_error_dev(self, "couldn't map interrupt\n");
		goto fail;
	}

	/*
	 * Allocate IRQ
	 */
	intrstr = pci_intr_string(pc, ih, intrbuf, sizeof(intrbuf));
	sc->sc_ih = pci_intr_establish(pc, ih, IPL_USB, xhci_intr, sc);
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "couldn't establish interrupt");
		if (intrstr != NULL)
			aprint_error(" at %s", intrstr);
		aprint_error("\n");
		goto fail;
	}
	aprint_normal_dev(self, "interrupting at %s\n", intrstr);

#if 0
	/* Figure out vendor for root hub descriptor. */
	vendor = pci_findvendor(pa->pa_id);
	sc->sc_id_vendor = PCI_VENDOR(pa->pa_id);
	if (vendor)
		strlcpy(sc->sc_vendor, vendor, sizeof(sc->sc_vendor));
	else
		snprintf(sc->sc_vendor, sizeof(sc->sc_vendor),
		    "vendor 0x%04x", PCI_VENDOR(pa->pa_id));
#endif

	err = xhci_init(sc);
	if (err) {
		aprint_error_dev(self, "init failed, error=%d\n", err);
		goto fail;
	}

	/* Intel chipset requires SuperSpeed enable and USB2 port routing */
	switch (PCI_VENDOR(pa->pa_id)) {
	case PCI_VENDOR_INTEL:
		xhci_pci_port_route(psc);
		break;
	default:
		break;
	}

	if (!pmf_device_register1(self, xhci_suspend, xhci_resume,
	                          xhci_shutdown))
		aprint_error_dev(self, "couldn't establish power handler\n");

	/* Attach usb device. */
	sc->sc_child = config_found(self, &sc->sc_bus, usbctlprint);
	return;

fail:
	if (sc->sc_ih) {
		pci_intr_disestablish(psc->sc_pc, sc->sc_ih);
		sc->sc_ih = NULL;
	}
	if (sc->sc_ios) {
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_ios);
		sc->sc_ios = 0;
	}
	return;
}

static int
xhci_pci_detach(device_t self, int flags)
{
	struct xhci_pci_softc * const psc = device_private(self);
	struct xhci_softc * const sc = &psc->sc_xhci;
	int rv;

	rv = xhci_detach(sc, flags);
	if (rv)
		return rv;

	pmf_device_deregister(self);

	xhci_shutdown(self, flags);

	if (sc->sc_ios) {
#if 0
		/* Disable interrupts, so we don't get any spurious ones. */
		bus_space_write_4(sc->sc_iot, sc->sc_ioh,
				  OHCI_INTERRUPT_DISABLE, OHCI_ALL_INTRS);
#endif
	}

	if (sc->sc_ih != NULL) {
		pci_intr_disestablish(psc->sc_pc, sc->sc_ih);
		sc->sc_ih = NULL;
	}
	if (sc->sc_ios) {
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_ios);
		sc->sc_ios = 0;
	}

	return 0;
}

CFATTACH_DECL3_NEW(xhci_pci, sizeof(struct xhci_pci_softc),
    xhci_pci_match, xhci_pci_attach, xhci_pci_detach, xhci_activate, NULL,
    xhci_childdet, DVF_DETACH_SHUTDOWN);
