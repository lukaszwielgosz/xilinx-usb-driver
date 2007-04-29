/* libusb/ppdev connector for XILINX impact
 *
 * Copyright (c) 2007 Michael Gernoth <michael@gernoth.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <usb.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <linux/parport.h>
#include <linux/ppdev.h>
#include "usb-driver.h"
#ifdef JTAGKEY
#include "jtagkey.h"
#endif

static int (*ioctl_func) (int, int, void *) = NULL;
static int windrvrfd = -1;
static int parportfd = -1;
static unsigned long ppbase = 0;
static unsigned long ecpbase = 0;
FILE *modulesfp = NULL;
FILE *baseaddrfp = NULL;
int baseaddrnum = 0;
static int modules_read = 0;
static struct usb_bus *busses = NULL;
static struct usb_device *usbdevice;
static usb_dev_handle *usb_devhandle = NULL;
static int usbinterface = -1;
static unsigned long card_type;
static int ints_enabled = 0;
static pthread_mutex_t int_wait = PTHREAD_MUTEX_INITIALIZER;

#define NO_WINDRVR 1

#ifdef DEBUG
void hexdump(unsigned char *buf, int len) {
	int i;

	for(i=0; i<len; i++) {
		fprintf(stderr,"%02x ", buf[i]);
		if ((i % 16) == 15)
			fprintf(stderr,"\n");
	}
	fprintf(stderr,"\n");
}
#endif

int usb_deviceinfo(unsigned char *buf) {
	int i,j,k,l;
	int len = 0;
	WDU_CONFIGURATION **pConfigs, **pActiveConfig;
	WDU_INTERFACE **pActiveInterface;

	if (buf) {
		struct usb_device_info *udi = (struct usb_device_info*)(buf+len);

		udi->Descriptor.bLength = sizeof(WDU_DEVICE_DESCRIPTOR);
		udi->Descriptor.bDescriptorType = usbdevice->descriptor.bDescriptorType;
		udi->Descriptor.bcdUSB = usbdevice->descriptor.bcdUSB;
		udi->Descriptor.bDeviceClass = usbdevice->descriptor.bDeviceClass;
		udi->Descriptor.bDeviceSubClass = usbdevice->descriptor.bDeviceSubClass;
		udi->Descriptor.bDeviceProtocol = usbdevice->descriptor.bDeviceProtocol;
		udi->Descriptor.bMaxPacketSize0 = usbdevice->descriptor.bMaxPacketSize0;
		udi->Descriptor.idVendor = usbdevice->descriptor.idVendor;
		udi->Descriptor.idProduct = usbdevice->descriptor.idProduct;
		udi->Descriptor.bcdDevice = usbdevice->descriptor.bcdDevice;
		udi->Descriptor.iManufacturer = usbdevice->descriptor.iManufacturer;
		udi->Descriptor.iProduct = usbdevice->descriptor.iProduct;
		udi->Descriptor.iSerialNumber = usbdevice->descriptor.iSerialNumber;
		udi->Descriptor.bNumConfigurations = usbdevice->descriptor.bNumConfigurations;

		/* TODO: Fix Pipe0! */
		udi->Pipe0.dwNumber = 0x00;
		udi->Pipe0.dwMaximumPacketSize = usbdevice->descriptor.bMaxPacketSize0;
		udi->Pipe0.type = 0;
		udi->Pipe0.direction = WDU_DIR_IN_OUT;
		udi->Pipe0.dwInterval = 0;

		pConfigs = &(udi->pConfigs);
		pActiveConfig = &(udi->pActiveConfig);
		pActiveInterface = &(udi->pActiveInterface[0]);
	}

	len = sizeof(struct usb_device_info);

	for (i=0; i<usbdevice->descriptor.bNumConfigurations; i++)
	{
		struct usb_config_descriptor *conf_desc = &usbdevice->config[i];
		WDU_INTERFACE **pInterfaces;
		WDU_ALTERNATE_SETTING **pAlternateSettings[conf_desc->bNumInterfaces];
		WDU_ALTERNATE_SETTING **pActiveAltSetting[conf_desc->bNumInterfaces];

		if (buf) {
			WDU_CONFIGURATION *cfg = (WDU_CONFIGURATION*)(buf+len);

			*pConfigs = cfg;
			*pActiveConfig = cfg;

			cfg->Descriptor.bLength = conf_desc->bLength;
			cfg->Descriptor.bDescriptorType = conf_desc->bDescriptorType;
			cfg->Descriptor.wTotalLength = conf_desc->wTotalLength;
			cfg->Descriptor.bNumInterfaces = conf_desc->bNumInterfaces;
			cfg->Descriptor.bConfigurationValue = conf_desc->bConfigurationValue;
			cfg->Descriptor.iConfiguration = conf_desc->iConfiguration;
			cfg->Descriptor.bmAttributes = conf_desc->bmAttributes;
			cfg->Descriptor.MaxPower = conf_desc->MaxPower;

			cfg->dwNumInterfaces = conf_desc->bNumInterfaces;

			pInterfaces = &(cfg->pInterfaces);
		}
		len += sizeof(WDU_CONFIGURATION);

		if (buf) {
			*pInterfaces = (WDU_INTERFACE*)(buf+len);
			for (j=0; j<conf_desc->bNumInterfaces; j++) {
				WDU_INTERFACE *iface = (WDU_INTERFACE*)(buf+len);

				pActiveInterface[j] = iface;

				pAlternateSettings[j] = &(iface->pAlternateSettings);
				iface->dwNumAltSettings = usbdevice->config[i].interface[j].num_altsetting;
				pActiveAltSetting[j] = &(iface->pActiveAltSetting);

				len += sizeof(WDU_INTERFACE);
			}
		} else {
			len += sizeof(WDU_INTERFACE) * conf_desc->bNumInterfaces;
		}

		for (j=0; j<conf_desc->bNumInterfaces; j++)
		{
			struct usb_interface *interface = &usbdevice->config[i].interface[j];

			if (buf) {
				*pAlternateSettings[j] = (WDU_ALTERNATE_SETTING*)(buf+len);
				/* FIXME: */
				*pActiveAltSetting[j] = (WDU_ALTERNATE_SETTING*)(buf+len);
			}

			for(k=0; k<interface->num_altsetting; k++)
			{
				unsigned char bNumEndpoints = interface->altsetting[k].bNumEndpoints;
				WDU_ENDPOINT_DESCRIPTOR **pEndpointDescriptors;
				WDU_PIPE_INFO **pPipes;

				if (buf) {
					WDU_ALTERNATE_SETTING *altset = (WDU_ALTERNATE_SETTING*)(buf+len);

					altset->Descriptor.bLength = interface->altsetting[k].bLength;
					altset->Descriptor.bDescriptorType = interface->altsetting[k].bDescriptorType;
					altset->Descriptor.bInterfaceNumber = interface->altsetting[k].bInterfaceNumber;
					altset->Descriptor.bAlternateSetting = interface->altsetting[k].bAlternateSetting;
					altset->Descriptor.bNumEndpoints = interface->altsetting[k].bNumEndpoints;
					altset->Descriptor.bInterfaceClass = interface->altsetting[k].bInterfaceClass;
					altset->Descriptor.bInterfaceSubClass = interface->altsetting[k].bInterfaceSubClass;
					altset->Descriptor.bInterfaceProtocol = interface->altsetting[k].bInterfaceProtocol;
					altset->Descriptor.iInterface = interface->altsetting[k].iInterface;
					pEndpointDescriptors = &(altset->pEndpointDescriptors);
					pPipes = &(altset->pPipes);

				}
				len +=sizeof(WDU_ALTERNATE_SETTING);

				if (buf) {
					*pEndpointDescriptors = (WDU_ENDPOINT_DESCRIPTOR*)(buf+len);
					for (l = 0; l < bNumEndpoints; l++) {
						WDU_ENDPOINT_DESCRIPTOR *ed = (WDU_ENDPOINT_DESCRIPTOR*)(buf+len);

						ed->bLength = interface->altsetting[k].endpoint[l].bLength;
						ed->bDescriptorType = interface->altsetting[k].endpoint[l].bDescriptorType;
						ed->bEndpointAddress = interface->altsetting[k].endpoint[l].bEndpointAddress;
						ed->bmAttributes = interface->altsetting[k].endpoint[l].bmAttributes;
						ed->wMaxPacketSize = interface->altsetting[k].endpoint[l].wMaxPacketSize;
						ed->bInterval = interface->altsetting[k].endpoint[l].bInterval;

						len += sizeof(WDU_ENDPOINT_DESCRIPTOR);
					}
						
					*pPipes = (WDU_PIPE_INFO*)(buf+len);
					for (l = 0; l < bNumEndpoints; l++) {
						WDU_PIPE_INFO *pi = (WDU_PIPE_INFO*)(buf+len);

						pi->dwNumber = interface->altsetting[k].endpoint[l].bEndpointAddress;
						pi->dwMaximumPacketSize = WDU_GET_MAX_PACKET_SIZE(interface->altsetting[k].endpoint[l].wMaxPacketSize);
						pi->type = interface->altsetting[k].endpoint[l].bmAttributes & USB_ENDPOINT_TYPE_MASK;
						if (pi->type == PIPE_TYPE_CONTROL)
							pi->direction = WDU_DIR_IN_OUT;
						else
						{
							pi->direction = interface->altsetting[k].endpoint[l].bEndpointAddress & USB_ENDPOINT_DIR_MASK ?  WDU_DIR_IN : WDU_DIR_OUT;
						}

						pi->dwInterval = interface->altsetting[k].endpoint[l].bInterval;

						len += sizeof(WDU_PIPE_INFO);
					}
				} else {
					len +=(sizeof(WDU_ENDPOINT_DESCRIPTOR)+sizeof(WDU_PIPE_INFO))*bNumEndpoints;
				}
			}
		}
	}

	return len;
}

int pp_transfer(WD_TRANSFER *tr, int fd, unsigned int request) {
	int ret = 0;
	unsigned long port = (unsigned long)tr->dwPort;
	unsigned char val;
	static unsigned char last_pp_write = 0;

	DPRINTF("dwPort: 0x%lx, cmdTrans: %lu, dwbytes: %ld, fautoinc: %ld, dwoptions: %ld\n",
			(unsigned long)tr->dwPort, tr->cmdTrans, tr->dwBytes,
			tr->fAutoinc, tr->dwOptions);

	val = tr->Data.Byte;
	
#ifdef DEBUG
	if (tr->cmdTrans == 13)
		DPRINTF("write byte: %d\n", val);
#endif

	if (parportfd < 0)
		return ret;

	if (port == ppbase + PP_DATA) {
		DPRINTF("data port\n");
		switch(tr->cmdTrans) {
			case PP_READ:
				ret = 0; /* We don't support reading of the data port */
				break;

			case PP_WRITE:
				ret = ioctl(parportfd, PPWDATA, &val);
				last_pp_write = val;
				break;

			default:
				fprintf(stderr,"!!!Unsupported TRANSFER command: %lu!!!\n", tr->cmdTrans);
				ret = -1;
				break;
		}
	} else if (port == ppbase + PP_STATUS) {
		DPRINTF("status port (last write: %d)\n", last_pp_write);
		switch(tr->cmdTrans) {
			case PP_READ:
				ret = ioctl(parportfd, PPRSTATUS, &val);
#ifdef FORCE_PC3_IDENT
				val &= 0x5f;
				if (last_pp_write & 0x40)
					val |= 0x20;
				else
					val |= 0x80;
#endif
				break;

			case PP_WRITE:
				ret = 0; /* Status Port is readonly */
				break;

			default:
				fprintf(stderr,"!!!Unsupported TRANSFER command: %lu!!!\n", tr->cmdTrans);
				ret = -1;
				break;
		}
	} else if (port == ppbase + PP_CONTROL) {
		DPRINTF("control port\n");
		switch(tr->cmdTrans) {
			case PP_READ:
				ret = ioctl(parportfd, PPRCONTROL, &val);
				break;

			case PP_WRITE:
				ret = ioctl(parportfd, PPWCONTROL, &val);
				break;

			default:
				fprintf(stderr,"!!!Unsupported TRANSFER command: %lu!!!\n", tr->cmdTrans);
				ret = -1;
				break;
		}
	} else if ((port == ecpbase + PP_ECP_CFGA) && ecpbase) {
		DPRINTF("ECP_CFGA port\n");
	} else if ((port == ecpbase + PP_ECP_CFGB) && ecpbase) {
		DPRINTF("ECP_CFGB port\n");
	} else if ((port == ecpbase + PP_ECP_ECR) && ecpbase) {
		DPRINTF("ECP_ECR port\n");
	} else {
		DPRINTF("access to unsupported address range!\n");
		ret = 0;
	}

	tr->Data.Byte = val;

	DPRINTF("dwPortReturn: 0x%lx, cmdTrans: %lu, dwbytes: %ld, fautoinc: %ld, dwoptions: %ld\n",
			(unsigned long)tr->dwPort, tr->cmdTrans, tr->dwBytes,
			tr->fAutoinc, tr->dwOptions);
#ifdef DEBUG
	if (tr->cmdTrans == 10)
		DPRINTF("read byte: %d\n", tr->Data.Byte);
#endif

	return ret;
}

int do_wdioctl(int fd, unsigned int request, unsigned char *wdioctl) {
	struct header_struct* wdheader = (struct header_struct*)wdioctl;
	struct version_struct *version;
	int ret = 0;

	if (wdheader->magic != MAGIC) {
		fprintf(stderr,"!!!ERROR: magic header does not match!!!\n");
		return (*ioctl_func) (fd, request, wdioctl);
	}

	switch(request & ~(0xc0000000)) {
		case VERSION:
			version = (struct version_struct*)(wdheader->data);
			strcpy(version->version, "libusb-driver.so $Revision: 1.63 $");
			version->versionul = 802;
			DPRINTF("VERSION\n");
			break;

		case LICENSE:
			DPRINTF("LICENSE\n");
			break;

		case CARD_REGISTER_OLD:
		case CARD_REGISTER:
			DPRINTF("CARD_REGISTER\n");
			{
				struct card_register* cr = (struct card_register*)(wdheader->data);
				char ppdev[32];

				DPRINTF("Items: %lu, Addr: 0x%lx, bytes: %lu, bar: %lu\n",
				cr->Card.dwItems,
				(unsigned long)cr->Card.Item[0].I.IO.dwAddr,
				cr->Card.Item[0].I.IO.dwBytes,
				cr->Card.Item[0].I.IO.dwBar);
				
				DPRINTF("Items: %lu, Addr: 0x%lx, bytes: %lu, bar: %lu\n",
				cr->Card.dwItems,
				(unsigned long)cr->Card.Item[1].I.IO.dwAddr,
				cr->Card.Item[1].I.IO.dwBytes,
				cr->Card.Item[1].I.IO.dwBar);
#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				
				/* FIXME: Ugly hack which maps amontec JtagKey to 4. parallel port */
#ifdef JTAGKEY
				if ((unsigned long)cr->Card.Item[0].I.IO.dwAddr == 0x30) {
					ret=jtagkey_init(0x0403, 0xcff8); /* I need a config file... */
					cr->hCard = 0xff;
					ppbase = (unsigned long)cr->Card.Item[0].I.IO.dwAddr;
					if (ret < 0)
						cr->hCard = 0;

					break;
				}
#endif
				if (parportfd < 0) {
					snprintf(ppdev, sizeof(ppdev), "/dev/parport%lu",
							(unsigned long)cr->Card.Item[0].I.IO.dwAddr / 0x10);
					DPRINTF("opening %s\n", ppdev);
					parportfd = open(ppdev, O_RDWR|O_EXCL);

					if (parportfd < 0)
						fprintf(stderr,"Can't open %s: %s\n", ppdev, strerror(errno));
				}

				if (parportfd >= 0) {
					int pmode;

					if (ioctl(parportfd, PPCLAIM) == -1)
						return ret;

					ecpbase = 0;
					pmode = IEEE1284_MODE_COMPAT;
					if (ioctl(parportfd, PPNEGOT, &pmode) == -1)
						return ret;

					if (cr->Card.dwItems > 1 && cr->Card.Item[1].I.IO.dwAddr) {
						DPRINTF("ECP mode requested\n");
						ecpbase = (unsigned long)cr->Card.Item[1].I.IO.dwAddr;
						/* TODO: Implement ECP mode */
#if 0
						pmode = IEEE1284_MODE_ECP;

						if (ioctl(parportfd, PPNEGOT, &pmode) == -1) {
							ecpbase = 0;
							pmode = IEEE1284_MODE_COMPAT;
							if (ioctl(parportfd, PPNEGOT, &pmode) == -1)
								return ret;
						}
#endif
					}

					cr->hCard = parportfd;
				}

				ppbase = (unsigned long)cr->Card.Item[0].I.IO.dwAddr;
				if (ret < 0)
					cr->hCard = 0;
#endif
				DPRINTF("hCard: %lu\n", cr->hCard);
			}
			break;

		case USB_TRANSFER:
			DPRINTF("in USB_TRANSFER");
			{
				struct usb_transfer *ut = (struct usb_transfer*)(wdheader->data);

#ifdef DEBUG
				DPRINTF(" unique: %lu, pipe: %lu, read: %lu, options: %lx, size: %lu, timeout: %lx\n",
				ut->dwUniqueID, ut->dwPipeNum, ut->fRead,
				ut->dwOptions, ut->dwBufferSize, ut->dwTimeout);
				DPRINTF("setup packet: ");
				hexdump(ut->SetupPacket, 8);

				if (!ut->fRead && ut->dwBufferSize)
				{
					hexdump(ut->pBuffer, ut->dwBufferSize);
				}
#endif

#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				/* http://www.jungo.com/support/documentation/windriver/802/wdusb_man_mhtml/node55.html#SECTION001213000000000000000 */
				if (ut->dwPipeNum == 0) { /* control pipe */
					int requesttype, request, value, index, size;
					requesttype = ut->SetupPacket[0];
					request = ut->SetupPacket[1];
					value = ut->SetupPacket[2] | (ut->SetupPacket[3] << 8);
					index = ut->SetupPacket[4] | (ut->SetupPacket[5] << 8);
					size = ut->SetupPacket[6] | (ut->SetupPacket[7] << 8);
					DPRINTF("requesttype: %x, request: %x, value: %u, index: %u, size: %u\n", requesttype, request, value, index, size);
					ret = usb_control_msg(usb_devhandle, requesttype, request, value, index, ut->pBuffer, size, ut->dwTimeout);
				} else {
					if (ut->fRead) {
						ret = usb_bulk_read(usb_devhandle, ut->dwPipeNum, ut->pBuffer, ut->dwBufferSize, ut->dwTimeout);

					} else {
						ret = usb_bulk_write(usb_devhandle, ut->dwPipeNum, ut->pBuffer, ut->dwBufferSize, ut->dwTimeout);
					}
				}

				if (ret < 0) {
					fprintf(stderr, "usb_transfer: %d (%s)\n", ret, usb_strerror());
				} else {
					ut->dwBytesTransferred = ret;
					ret = 0;
				}
#endif

#ifdef DEBUG
				DPRINTF("Transferred: %lu (%s)\n",ut->dwBytesTransferred, (ut->fRead?"read":"write"));
				if (ut->fRead && ut->dwBytesTransferred)
				{
					DPRINTF("Read: ");
					hexdump(ut->pBuffer, ut->dwBytesTransferred);
				}
#endif
			}
			break;

		case INT_ENABLE_OLD:
		case INT_ENABLE:
			DPRINTF("INT_ENABLE\n");
			{
				struct interrupt *it = (struct interrupt*)(wdheader->data);

				DPRINTF("Handle: %lu, Options: %lx, ncmds: %lu, enableok: %lu, count: %lu, lost: %lu, stopped: %lu\n",
				it->hInterrupt, it->dwOptions,
				it->dwCmds, it->fEnableOk, it->dwCounter,
				it->dwLost, it->fStopped);

				it->fEnableOk = 1;
				it->fStopped = 0;
				ints_enabled = 1;
				pthread_mutex_trylock(&int_wait);
			}

			break;
			
		case INT_DISABLE:
			DPRINTF("INT_DISABLE\n");
			{
				struct interrupt *it = (struct interrupt*)(wdheader->data);

				DPRINTF("Handle: %lu, Options: %lx, ncmds: %lu, enableok: %lu, count: %lu, lost: %lu, stopped: %lu\n",
				it->hInterrupt, it->dwOptions,
				it->dwCmds, it->fEnableOk, it->dwCounter,
				it->dwLost, it->fStopped);
#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				it->dwCounter = 0;
				it->fStopped = 1;
				ints_enabled = 0;
				if (pthread_mutex_trylock(&int_wait) == EBUSY)
					pthread_mutex_unlock(&int_wait);
#endif
				DPRINTF("Handle: %lu, Options: %lx, ncmds: %lu, enableok: %lu, count: %lu, lost: %lu, stopped: %lu\n",
				it->hInterrupt, it->dwOptions,
				it->dwCmds, it->fEnableOk, it->dwCounter,
				it->dwLost, it->fStopped);
			}
			break;

		case USB_SET_INTERFACE:
			DPRINTF("USB_SET_INTERFACE\n");
			{
				struct usb_set_interface *usi = (struct usb_set_interface*)(wdheader->data);

				DPRINTF("unique: %lu, interfacenum: %lu, alternatesetting: %lu, options: %lx\n",
				usi->dwUniqueID, usi->dwInterfaceNum,
				usi->dwAlternateSetting, usi->dwOptions);
#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				if (usbdevice) {
					if (!usb_devhandle)
						usb_devhandle = usb_open(usbdevice);

					/* FIXME: Select right interface! */
					ret = usb_claim_interface(usb_devhandle, usbdevice->config[0].interface[usi->dwInterfaceNum].altsetting[usi->dwAlternateSetting].bInterfaceNumber);
					if (!ret) {
						if(!ret) {
							usbinterface = usbdevice->config[0].interface[usi->dwInterfaceNum].altsetting[usi->dwAlternateSetting].bInterfaceNumber;
							ret = usb_set_altinterface(usb_devhandle, usi->dwAlternateSetting);
							if (ret)
								fprintf(stderr, "usb_set_altinterface: %d\n", ret);
						} else {
							fprintf(stderr, "usb_set_configuration: %d (%s)\n", ret, usb_strerror());
						}
					} else {
						fprintf(stderr, "usb_claim_interface: %d -> %d (%s)\n",
						usbdevice->config[0].interface[usi->dwInterfaceNum].altsetting[usi->dwAlternateSetting].bInterfaceNumber,
						ret, usb_strerror());
					}
				}
#endif
				DPRINTF("unique: %lu, interfacenum: %lu, alternatesetting: %lu, options: %lx\n",
				usi->dwUniqueID, usi->dwInterfaceNum,
				usi->dwAlternateSetting, usi->dwOptions);
			}
			break;

		case USB_GET_DEVICE_DATA_OLD:
		case USB_GET_DEVICE_DATA:
			DPRINTF("USB_GET_DEVICE_DATA\n");
			{
				struct usb_get_device_data *ugdd = (struct usb_get_device_data*)(wdheader->data);
				int pSize;

				DPRINTF("unique: %lu, bytes: %lu, options: %lx\n",
				ugdd->dwUniqueID, ugdd->dwBytes,
				ugdd->dwOptions);

				pSize = ugdd->dwBytes;
				if (!ugdd->dwBytes) {
					if (usbdevice) {
						ugdd->dwBytes = usb_deviceinfo(NULL);
					}
				} else {
					usb_deviceinfo((unsigned char*)ugdd->pBuf);
				}
			}
			break;

		case EVENT_REGISTER_OLD:
		case EVENT_REGISTER:
			DPRINTF("EVENT_REGISTER\n");
			{
				struct event *e = (struct event*)(wdheader->data);
				struct usb_bus *bus;
				int i;

				DPRINTF("handle: %lu, action: %lu, status: %lu, eventid: %lu, cardtype: %lu, kplug: %lu, options: %lu, dev: %lx:%lx, unique: %lu, ver: %lu, nummatch: %lu\n",
				e->handle, e->dwAction,
				e->dwStatus, e->dwEventId, e->dwCardType,
				e->hKernelPlugIn, e->dwOptions,
				e->u.Usb.deviceId.dwVendorId,
				e->u.Usb.deviceId.dwProductId,
				e->u.Usb.dwUniqueID, e->dwEventVer,
				e->dwNumMatchTables);

				for (i = 0; i < e->dwNumMatchTables; i++) {

					DPRINTF("match: dev: %04x:%04x, class: %x, subclass: %x, intclass: %x, intsubclass: %x, intproto: %x\n",
					e->matchTables[i].VendorId,
					e->matchTables[i].ProductId,
					e->matchTables[i].bDeviceClass,
					e->matchTables[i].bDeviceSubClass,
					e->matchTables[i].bInterfaceClass,
					e->matchTables[i].bInterfaceSubClass,
					e->matchTables[i].bInterfaceProtocol);

					for (bus = busses; bus; bus = bus->next) {
						struct usb_device *dev;

						for (dev = bus->devices; dev; dev = dev->next) {
							struct usb_device_descriptor *desc = &(dev->descriptor);

							if((desc->idVendor == e->matchTables[i].VendorId) &&
							   (desc->idProduct == e->matchTables[i].ProductId) &&
							   (desc->bDeviceClass == e->matchTables[i].bDeviceClass) &&
							   (desc->bDeviceSubClass == e->matchTables[i].bDeviceSubClass)) {
								   int ac;
								   for (ac = 0; ac < desc->bNumConfigurations; ac++) {
									   struct usb_interface *interface = dev->config[ac].interface;
									   int ai;

									   for (ai = 0; ai < interface->num_altsetting; ai++) {

										   DPRINTF("intclass: %x, intsubclass: %x, intproto: %x\n",
										   interface->altsetting[i].bInterfaceClass,
										   interface->altsetting[i].bInterfaceSubClass,
										   interface->altsetting[i].bInterfaceProtocol);

										   if ((interface->altsetting[ai].bInterfaceSubClass == e->matchTables[i].bInterfaceSubClass) &&
												   (interface->altsetting[ai].bInterfaceProtocol == e->matchTables[i].bInterfaceProtocol)){
											   /* TODO: check interfaceClass! */
											   DPRINTF("found device with libusb\n");
											   usbdevice = dev;
											   card_type = e->dwCardType;
										   }
									   }
								   }
							}
						}
					}
				}

#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				e->handle++;
#endif

#ifdef DEBUG
				DPRINTF("handle: %lu, action: %lu, status: %lu, eventid: %lu, cardtype: %lu, kplug: %lu, options: %lu, dev: %lx:%lx, unique: %lu, ver: %lu, nummatch: %lu\n",
				e->handle, e->dwAction,
				e->dwStatus, e->dwEventId, e->dwCardType,
				e->hKernelPlugIn, e->dwOptions,
				e->u.Usb.deviceId.dwVendorId,
				e->u.Usb.deviceId.dwProductId,
				e->u.Usb.dwUniqueID, e->dwEventVer,
				e->dwNumMatchTables);

				for (i = 0; i < e->dwNumMatchTables; i++)
					DPRINTF("match: dev: %04x:%04x, class: %x, subclass: %x, intclass: %x, intsubclass: %x, intproto: %x\n",
					e->matchTables[i].VendorId,
					e->matchTables[i].ProductId,
					e->matchTables[i].bDeviceClass,
					e->matchTables[i].bDeviceSubClass,
					e->matchTables[i].bInterfaceClass,
					e->matchTables[i].bInterfaceSubClass,
					e->matchTables[i].bInterfaceProtocol);
#endif
			}
			break;

		case TRANSFER_OLD:
		case TRANSFER:
			DPRINTF("TRANSFER\n");
			{
				WD_TRANSFER *tr = (WD_TRANSFER*)(wdheader->data);

#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else

#ifdef JTAGKEY
				if (ppbase == 0x30) {
					ret = jtagkey_transfer(tr, fd, request, ppbase, ecpbase, 1);
					break;
				}
#endif /* JTAGKEY */
				ret = pp_transfer(tr, fd, request);
#endif
			}
			break;

		case MULTI_TRANSFER_OLD:
		case MULTI_TRANSFER:
			DPRINTF("MULTI_TRANSFER\n");
			{
				WD_TRANSFER *tr = (WD_TRANSFER*)(wdheader->data);
				unsigned long num = wdheader->size/sizeof(WD_TRANSFER);
				int i;
#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else

#ifdef JTAGKEY
				/* FIXME: Config file and mor intelligent mapping! */
				if (ppbase == 0x30) {
					ret = jtagkey_transfer(tr, fd, request, ppbase, ecpbase, num);
					break;
				}
#endif /* JTAGKEY */

				for (i = 0; i < num; i++) {
					DPRINTF("Transfer %d:\n", i+1);
					ret = pp_transfer(tr + i, fd, request);
				}
#endif
			}
			break;

		case EVENT_UNREGISTER:
			DPRINTF("EVENT_UNREGISTER\n");
#ifndef NO_WINDRVR
			ret = (*ioctl_func) (fd, request, wdioctl);
#endif
			break;

		case INT_WAIT:
			DPRINTF("INT_WAIT\n");
			{
				struct interrupt *it = (struct interrupt*)(wdheader->data);

				DPRINTF("Handle: %lu, Options: %lx, ncmds: %lu, enableok: %lu, count: %lu, lost: %lu, stopped: %lu\n",
				it->hInterrupt, it->dwOptions,
				it->dwCmds, it->fEnableOk, it->dwCounter,
				it->dwLost, it->fStopped);

#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				if (usbdevice) {
					if (it->dwCounter == 0) {
						it->dwCounter = 1;
					} else {
						pthread_mutex_lock(&int_wait);
						pthread_mutex_unlock(&int_wait);
					}
				} else {
					pthread_mutex_lock(&int_wait);
					pthread_mutex_unlock(&int_wait);
				}
#endif

				DPRINTF("INT_WAIT_RETURN: Handle: %lu, Options: %lx, ncmds: %lu, enableok: %lu, count: %lu, lost: %lu, stopped: %lu\n",
				it->hInterrupt, it->dwOptions, it->dwCmds,
				it->fEnableOk, it->dwCounter, it->dwLost,
				it->fStopped);
			}
			break;

		case CARD_UNREGISTER:
			DPRINTF("CARD_UNREGISTER\n");
			{
				struct card_register* cr = (struct card_register*)(wdheader->data);

				DPRINTF("Addr: 0x%lx, bytes: %lu, bar: %lu\n",
				(unsigned long)cr->Card.Item[0].I.IO.dwAddr,
				cr->Card.Item[0].I.IO.dwBytes,
				cr->Card.Item[0].I.IO.dwBar);

				DPRINTF("hCard: %lu\n", cr->hCard);

#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				if (parportfd == cr->hCard && parportfd >= 0) {
					ioctl(parportfd, PPRELEASE);
					close(parportfd);
					parportfd = -1;
				}
#endif
			}
			break;

		case EVENT_PULL:
			DPRINTF("EVENT_PULL\n");
			{
				struct event *e = (struct event*)(wdheader->data);
#ifdef DEBUG
				int i;

				DPRINTF("handle: %lu, action: %lu, status: %lu, eventid: %lu, cardtype: %lx, kplug: %lu, options: %lu, dev: %lx:%lx, unique: %lu, ver: %lu, nummatch: %lu\n",
				e->handle, e->dwAction, e->dwStatus,
				e->dwEventId, e->dwCardType, e->hKernelPlugIn,
				e->dwOptions, e->u.Usb.deviceId.dwVendorId,
				e->u.Usb.deviceId.dwProductId,
				e->u.Usb.dwUniqueID, e->dwEventVer,
				e->dwNumMatchTables);

				for (i = 0; i < e->dwNumMatchTables; i++)
					DPRINTF("match: dev: %04x:%04x, class: %x, subclass: %x, intclass: %x, intsubclass: %x, intproto: %x\n",
					e->matchTables[i].VendorId,
					e->matchTables[i].ProductId,
					e->matchTables[i].bDeviceClass,
					e->matchTables[i].bDeviceSubClass,
					e->matchTables[i].bInterfaceClass,
					e->matchTables[i].bInterfaceSubClass,
					e->matchTables[i].bInterfaceProtocol);
#endif

#ifndef NO_WINDRVR
				ret = (*ioctl_func) (fd, request, wdioctl);
#else
				if (usbdevice) {
					struct usb_interface *interface = usbdevice->config->interface;

					e->dwCardType = card_type;
					e->dwAction = 1;
					e->dwEventId = 109;
					e->u.Usb.dwUniqueID = 110;
					e->matchTables[0].VendorId = usbdevice->descriptor.idVendor;
					e->matchTables[0].ProductId = usbdevice->descriptor.idProduct;
					e->matchTables[0].bDeviceClass = usbdevice->descriptor.bDeviceClass;
					e->matchTables[0].bDeviceSubClass = usbdevice->descriptor.bDeviceSubClass;
					e->matchTables[0].bInterfaceClass = interface->altsetting[0].bInterfaceClass;
					e->matchTables[0].bInterfaceSubClass = interface->altsetting[0].bInterfaceSubClass;
					e->matchTables[0].bInterfaceProtocol = interface->altsetting[0].bInterfaceProtocol;
				}
#endif

#ifdef DEBUG
				DPRINTF("handle: %lu, action: %lu, status: %lu, eventid: %lu, cardtype: %lx, kplug: %lu, options: %lu, dev: %lx:%lx, unique: %lu, ver: %lu, nummatch: %lu\n",
				e->handle, e->dwAction, e->dwStatus,
				e->dwEventId, e->dwCardType, e->hKernelPlugIn,
				e->dwOptions, e->u.Usb.deviceId.dwVendorId,
				e->u.Usb.deviceId.dwProductId,
				e->u.Usb.dwUniqueID, e->dwEventVer,
				e->dwNumMatchTables);

				for (i = 0; i < e->dwNumMatchTables; i++)
					DPRINTF("match: dev: %04x:%04x, class: %x, subclass: %x, intclass: %x, intsubclass: %x, intproto: %x\n",
					e->matchTables[i].VendorId,
					e->matchTables[i].ProductId,
					e->matchTables[i].bDeviceClass,
					e->matchTables[i].bDeviceSubClass,
					e->matchTables[i].bInterfaceClass,
					e->matchTables[i].bInterfaceSubClass,
					e->matchTables[i].bInterfaceProtocol);
#endif

			}
			break;

		default:
			fprintf(stderr,"!!!Unsupported IOCTL: %x!!!\n", request);
#ifndef NO_WINDRVR
			ret = (*ioctl_func) (fd, request, wdioctl);
#endif
			break;
	}

	return ret;
}

int ioctl(int fd, unsigned long int request, ...) {
	va_list args;
	void *argp;
	int ret;

	if (!ioctl_func)                                                                    
		ioctl_func = (int (*) (int, int, void *)) dlsym (RTLD_NEXT, "ioctl");             

	va_start (args, request);
	argp = va_arg (args, void *);
	va_end (args);

	if (fd == windrvrfd)
		ret = do_wdioctl(fd, request, argp);
	else
		ret = (*ioctl_func) (fd, request, argp);

	return ret;
}

int open (const char *pathname, int flags, ...) {
	static int (*func) (const char *, int, mode_t) = NULL;
	mode_t mode = 0;
	va_list args;
	int fd;

	if (!func)
		func = (int (*) (const char *, int, mode_t)) dlsym (RTLD_NEXT, "open");

	if (flags & O_CREAT) {
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		va_end(args);
	}

	if (!strcmp (pathname, "/dev/windrvr6")) {
		DPRINTF("opening windrvr6\n");
#ifdef NO_WINDRVR
		windrvrfd = fd = (*func) ("/dev/null", flags, mode);
#else
		windrvrfd = fd = (*func) (pathname, flags, mode);
#endif
		if (!busses) {
			usb_init();
			usb_find_busses();
			usb_find_devices();

			busses = usb_get_busses();
		}

		return fd;
	}

	return (*func) (pathname, flags, mode);
}

int close(int fd) {
	static int (*func) (int) = NULL;

	if (!func)
		func = (int (*) (int)) dlsym(RTLD_NEXT, "close");
	
	if (fd == windrvrfd && windrvrfd >= 0) {
		DPRINTF("close windrvrfd\n");
		if (usbinterface >= 0)
			usb_release_interface(usb_devhandle, usbinterface);

		if (usb_devhandle)
			usb_close(usb_devhandle);

		usb_devhandle = NULL;
		usbinterface = -1;
		windrvrfd = -1;
	}

	return (*func) (fd);
}

FILE *fopen(const char *path, const char *mode) {
	FILE *ret;
	static FILE* (*func) (const char*, const char*) = NULL;
	char buf[256];
	int i;

	if (!func)
		func = (FILE* (*) (const char*, const char*)) dlsym(RTLD_NEXT, "fopen");

#ifdef JTAGKEY
	/* FIXME: Hack for parport mapping */
	if (!strcmp(path, "/proc/sys/dev/parport/parport3/base-addr")) {
		ret = (*func) ("/dev/null", mode);
	} else
#endif
		ret = (*func) (path, mode);

	if (!strcmp(path, "/proc/modules")) {
		DPRINTF("opening /proc/modules\n");
#ifdef NO_WINDRVR
		modulesfp = ret;
		modules_read = 0;
#endif
	}
	
	if (ret) {
		for (i = 0; i < 4; i++) {
			snprintf(buf, sizeof(buf), "/proc/sys/dev/parport/parport%d/base-addr", i);
			if (!strcmp(path, buf)) {
				DPRINTF("open base-addr of parport%d\n", i);
				baseaddrfp = ret;
				baseaddrnum = i;
			}
		}
	}

	return ret;
}

char *fgets(char *s, int size, FILE *stream) {
        static char* (*func) (char*, int, FILE*) = NULL;
	const char modules[][256] = {"windrvr6 1 0 - Live 0xdeadbeef\n", "parport_pc 1 0 - Live 0xdeadbeef\n"};
	char buf[256];
	char *ret = NULL;


	if (!func)
		func = (char* (*) (char*, int, FILE*)) dlsym(RTLD_NEXT, "fgets");
	
	if (modulesfp == stream) {
		if (modules_read < sizeof(modules) / sizeof(modules[0])) {
			strcpy(s, modules[modules_read]);
			ret = s;
			modules_read++;
		}
	} else if (baseaddrfp == stream) {
		snprintf(s, sizeof(buf), "%d\t%d\n",
			(baseaddrnum) * 0x10,
			((baseaddrnum) * 0x10) + 0x400);
		ret = s;
	} else {
		ret = (*func)(s,size,stream);
	}

	return ret;
}

int fclose(FILE *fp) {
	static int (*func) (FILE*) = NULL;

	if (!func)
		func = (int (*) (FILE*)) dlsym(RTLD_NEXT, "fclose");

	if (fp == modulesfp) {
		modulesfp = NULL;
	}

	if (fp == baseaddrfp) {
		baseaddrfp = NULL;
	}
	
	return (*func)(fp);
}

int access(const char *pathname, int mode) {
	static int (*func) (const char*, int);

	if (!func)
		func = (int (*) (const char*, int)) dlsym(RTLD_NEXT, "access");
	
	if (!strcmp(pathname, "/dev/windrvr6")) {
		return 0;
	} else {
		return (*func)(pathname, mode);
	}
}
