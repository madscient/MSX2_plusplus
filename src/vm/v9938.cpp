// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Nathan Woods

/***************************************************************************

v9938 / v9958 emulation

Vertical display parameters from Yamaha V9938 Technical Data Book.
NTSC: page 146, Table 7-2
PAL: page 147, Table 7-3

Vertical timing:
                                       PAL                 NTSC
                                       192(LN=0) 212(LN=1) 192(LN=0) 212(LN=1)
                                       ------------------- --------------------
1. Top erase (top blanking)                13        13        13        13
2. Top border                              53        43        26        16
3. Active display                         192       212       192       212
4. Bottom border                           49        39        25        15
5. Bottom erase (bottom blanking)           3         3         3         3
6. Vertical sync (bottom blanking)          3         3         3         3
7. Total                                  313       313       262       262

   Refresh rate                           50.158974           59.922743


***************************************************************************/

/*
todo:

- sprite collision
- vdp engine -- make run at correct speed
- vr/hr/fh flags: double-check all of that
- make vdp engine work in exp. ram
*/

#include "../emu.h"
#include "v9938.h"

#define m_vram_space this

#define VERBOSE 0
//#define LOG(x)  do { if (VERBOSE) logerror x; } while (0)
#define LOG(x)  void(x)

#define TP_DIS	(m_cont_reg[8] & 0x20)
#define GET_TP(x)	((x || TP_DIS) ? (-1) : 0)

enum
{
	V9938_MODE_TEXT1 = 0,
	V9938_MODE_MULTI,
	V9938_MODE_GRAPHIC1,
	V9938_MODE_GRAPHIC2,
	V9938_MODE_GRAPHIC3,
	V9938_MODE_GRAPHIC4,
	V9938_MODE_GRAPHIC5,
	V9938_MODE_GRAPHIC6,
	V9938_MODE_GRAPHIC7,
	V9938_MODE_TEXT2,
	V9938_MODE_UNKNOWN
};

#define MODEL_V9938 (0)
#define MODEL_V9958 (1)

#define EXPMEM_OFFSET 0x20000

#define LONG_WIDTH (512 + 32)

static const char *const v9938_modes[] = {
	"TEXT 1", "MULTICOLOR", "GRAPHIC 1", "GRAPHIC 2", "GRAPHIC 3",
	"GRAPHIC 4", "GRAPHIC 5", "GRAPHIC 6", "GRAPHIC 7", "TEXT 2",
	"UNKNOWN"
};

//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************

/*
Similar to the TMS9928, the V9938 has an own address space. It can handle
at most 192 KiB RAM (128 KiB base, 64 KiB expansion).
*/
//static ADDRESS_MAP_START(memmap, AS_DATA, 8, v99x8_device)
//	ADDRESS_MAP_GLOBAL_MASK(0x3ffff)
//	AM_RANGE(0x00000, 0x2ffff) AM_RAM
//ADDRESS_MAP_END


// devices
//const device_type V9938 = &device_creator<v9938_device>;
//const device_type V9958 = &device_creator<v9958_device>;


//v99x8_device::v99x8_device(const machine_config &mconfig, device_type type, const char *name, const char *shortname, const char *tag, device_t *owner, UINT32 clock)
//:   device_t(mconfig, type, name, tag, owner, clock, shortname, __FILE__),
v99x8_device::v99x8_device(VM_TEMPLATE* parent_vm, EMU* parent_emu)
:   DEVICE(parent_vm, parent_emu),
//	device_memory_interface(mconfig, *this),
//	device_video_interface(mconfig, *this),
//	m_space_config("vram", ENDIANNESS_BIG, 8, 18),
	m_model(0),
	m_offset_x(0),
	m_offset_y(0),
	m_visible_y(0),
	m_mode(0),
	m_pal_write_first(0),
	m_cmd_write_first(0),
	m_pal_write(0),
	m_cmd_write(0),
	m_read_ahead(0),
	m_v9958_sp_mode(0),
	m_address_latch(0),
	m_vram_size(/*0*/0x20000),
	m_int_state(0),
//	m_int_callback(*this),
	m_scanline(0),
	m_blink(0),
	m_blink_count(0),
	m_mx_delta(0),
	m_my_delta(0),
	m_button_state(0),
	m_vdp_ops_count(0),
	m_vdp_engine(NULL),
//	m_palette(*this, "palette"),
	m_pal_ntsc(0)
{
//	static_set_addrmap(*this, AS_DATA, ADDRESS_MAP_NAME(memmap));
	initialize_output_signals(&outputs_irq);
	set_device_name(_T("V99x8 VDP"));
}

//v9938_device::v9938_device(const machine_config &mconfig, const char *tag, device_t *owner, UINT32 clock)
//: v99x8_device(mconfig, V9938, "V9938 VDP", "v9938", tag, owner, clock)
v9938_device::v9938_device(VM_TEMPLATE* parent_vm, EMU* parent_emu)
: v99x8_device(parent_vm, parent_emu)
{
	m_model = MODEL_V9938;
	init_palette();
	set_device_name(_T("V9938 VDP"));
}

//v9958_device::v9958_device(const machine_config &mconfig, const char *tag, device_t *owner, UINT32 clock)
//: v99x8_device(mconfig, V9938, "V9958 VDP", "v9958", tag, owner, clock)
v9958_device::v9958_device(VM_TEMPLATE* parent_vm, EMU* parent_emu)
: v99x8_device(parent_vm, parent_emu)
{
	m_model = MODEL_V9958;
	init_palette();
	set_device_name(_T("V9958 VDP"));
}


//void v99x8_device::device_timer(emu_timer &timer, device_timer_id id, int param, void *ptr)
void v99x8_device::device_timer(int v)
{
	int scanline = (m_scanline - (m_scanline_start + m_offset_y));

	update_command ();

	// set flags
	if (m_scanline == (m_scanline_start + m_offset_y))
	{
		m_stat_reg[2] &= ~0x40;
	}
	else if (m_scanline == (m_scanline_start + m_offset_y + m_visible_y))
	{
		m_stat_reg[2] |= 0x40;
		m_stat_reg[0] |= 0x80;
	}

	if ( (scanline >= 0) && (scanline <= m_scanline_max) &&
		(((scanline + m_cont_reg[23]) & 255) == m_cont_reg[19]) )
	{
		m_stat_reg[1] |= 1;
		LOG(("V9938: scanline interrupt (%d)\n", scanline));
	}
	else if (!(m_cont_reg[0] & 0x10))
	{
		m_stat_reg[1] &= 0xfe;
	}

	check_int();

	// check for start of vblank
	if (m_scanline == m_vblank_start)
	{
		interrupt_start_vblank();
	}

	// render the current line
	if (m_scanline < m_vblank_start)
	{
		refresh_line(scanline);
	}

	if (++m_scanline >= m_height)
	{
		m_scanline = 0;
		// PAL/NTSC changed?
		/*int pal = m_cont_reg[9] & 2;
		if (m_pal_ntsc != pal)
		{
			m_pal_ntsc = pal;
			configure_pal_ntsc();
		}*/ /* umaiboux: NTSC only! */
		//m_screen->reset_origin();
		m_offset_y = position_offset(m_cont_reg[18] >> 4);
		set_screen_parameters();
	}
}


void v99x8_device::set_screen_parameters()
{
	if (m_pal_ntsc)
	{
		// PAL
		m_scanline_start = (m_cont_reg[9] & 0x80) ? 43 : 53;
		m_scanline_max = 255;
	}
	else
	{
		// NYSC
		m_scanline_start = (m_cont_reg[9] & 0x80) ? 16 : 26;
		m_scanline_max = (m_cont_reg[9] & 0x80) ? 234 : 244;
	}
	m_visible_y = (m_cont_reg[9] & 0x80) ? 212 : 192;
}


void v99x8_device::configure_pal_ntsc()
{
	if (m_pal_ntsc)
	{
		// PAL
		m_height = VTOTAL_PAL;
//		rectangle visible;
//		visible.set(0, HVISIBLE - 1, VERTICAL_ADJUST * 2, VVISIBLE_PAL * 2 - 1 - VERTICAL_ADJUST * 2);
//		m_screen->configure(HTOTAL, VTOTAL_PAL * 2, visible, HZ_TO_ATTOSECONDS(50.158974));
	}
	else
	{
		// NTSC
		m_height = VTOTAL_NTSC;
//		rectangle visible;
//		visible.set(0, HVISIBLE - 1, VERTICAL_ADJUST * 2, VVISIBLE_NTSC * 2 - 1 - VERTICAL_ADJUST * 2);
//		m_screen->configure(HTOTAL, VTOTAL_NTSC * 2, visible, HZ_TO_ATTOSECONDS(59.922743));
	}
	m_vblank_start = m_height - VERTICAL_SYNC - TOP_ERASE; /* Sync + top erase */
}


/*
    Not really right... won't work with sprites in graphics 7
    and with palette updated mid-screen
*/
int v99x8_device::get_transpen()
{
	if (m_mode == V9938_MODE_GRAPHIC7)
	{
		return m_pal_ind256[0];
	}
	else
	{
		return m_pal_ind16[0];
	}
}

/*
    Driver-specific function: update the vdp mouse state
*/
/*void v99x8_device::update_mouse_state(int mx_delta, int my_delta, int button_state)
{
	// save button state
	m_button_state = (button_state << 6) & 0xc0;

	if ((m_cont_reg[8] & 0xc0) == 0x80)
	{   // vdp will process mouse deltas only if it is in mouse mode
		m_mx_delta += mx_delta;
		m_my_delta += my_delta;
	}
}*/



/***************************************************************************

Palette functions

***************************************************************************/

/*
About the colour burst registers:

The color burst registers will only have effect on the composite video output from
the V9938. but the output is only NTSC (Never The Same Color ,so the
effects are already present) . this system is not used in europe
the european machines use a separate PAL  (Phase Alternating Line) encoder
or no encoder at all , only RGB output.

Erik de Boer.

--
Right now they're not emulated. For completeness sake they should -- with
a dip-switch to turn them off. I really don't know how they work though. :(
*/

/*
In screen 8, the colors are encoded as:

7  6  5  4  3  2  1  0
+--+--+--+--+--+--+--+--+
|g2|g1|g0|r2|r1|r0|b2|b1|
+--+--+--+--+--+--+--+--+

b0 is set if b2 and b1 are set (remember, color bus is 3 bits)

*/

/*PALETTE_INIT_MEMBER(v9938_device, v9938)*/
void v9938_device::init_palette()
{
	int i;

	// create the full 512 colour palette
	for (i=0;i<512;i++)
//		palette.set_pen_color(i, pal3bit(i >> 6), pal3bit(i >> 3), pal3bit(i >> 0));
		this->set_pen_color(i, pal3bit(i >> 6), pal3bit(i >> 3), pal3bit(i >> 0));
}

/*

The v9958 can display up to 19286 colours. For this we need a larger palette.

The colours are encoded in 17 bits; however there are just 19268 different colours.
Here we calculate the palette and a 2^17 reference table to the palette,
which is: s_pal_indYJK. It's 256K in size, but I can't think of a faster way
to emulate this. Also it keeps the palette a reasonable size. :)

*/

UINT16 v99x8_device::s_pal_indYJK[0x20000];

/*PALETTE_INIT_MEMBER(v9958_device, v9958)*/
void v9958_device::init_palette()
{
	int r,g,b,y,j,k,i,k0,j0,n;
	UINT8 pal[19268*3];

	// init v9938 512-color palette
	for (i=0;i<512;i++)
//		palette.set_pen_color(i, pal3bit(i >> 6), pal3bit(i >> 3), pal3bit(i >> 0));
		this->set_pen_color(i, pal3bit(i >> 6), pal3bit(i >> 3), pal3bit(i >> 0));


//	if(palette.entries() != 19780)
//		fatalerror("V9958: not enough palette, must be 19780");

	// set up YJK table
	LOG(("Building YJK table for V9958 screens, may take a while ... \n"));
	i = 0;
	for (y=0;y<32;y++) for (k=0;k<64;k++) for (j=0;j<64;j++)
	{
		// calculate the color
		if (k >= 32) k0 = (k - 64); else k0 = k;
		if (j >= 32) j0 = (j - 64); else j0 = j;
		r = y + j0;
		b = (y * 5 - 2 * j0 - k0) / 4;
		g = y + k0;
		if (r < 0) r = 0; else if (r > 31) r = 31;
		if (g < 0) g = 0; else if (g > 31) g = 31;
		if (b < 0) b = 0; else if (b > 31) b = 31;

		//r = (r << 3) | (r >> 2);
		//b = (b << 3) | (b >> 2);
		//g = (g << 3) | (g >> 2);
		// have we seen this one before?
		n = 0;
		while (n < i)
		{
			if (pal[n*3+0] == r && pal[n*3+1] == g && pal[n*3+2] == b)
			{
				v99x8_device::s_pal_indYJK[y | j << 5 | k << (5 + 6)] = n + 512;
				break;
			}
			n++;
		}

		if (i == n)
		{
			// so we haven't; add it
			pal[i*3+0] = r;
			pal[i*3+1] = g;
			pal[i*3+2] = b;
//			palette.set_pen_color(i+512, rgb_t(pal5bit(r), pal5bit(g), pal5bit(b)));
			this->set_pen_color(i+512, pal5bit(r), pal5bit(g), pal5bit(b));
			v99x8_device::s_pal_indYJK[y | j << 5 | k << (5 + 6)] = i + 512;
			i++;
		}
	}

	if (i != 19268)
		LOG( ("Table creation failed - %d colours out of 19286 created\n", i));
}

/*UINT32 v99x8_device::screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	copybitmap(bitmap, m_bitmap, 0, 0, 0, 0, cliprect);
	return 0;
}*/

/*READ8_MEMBER( v99x8_device::read )*/
uint32_t v99x8_device::read_io8(uint32_t offset)
{
	switch (offset & 3)
	{
	case 0: return vram_r();
	case 1: return status_r();
	}
	return 0xff;
}

/*WRITE8_MEMBER( v99x8_device::write )*/
void v99x8_device::write_io8(uint32_t offset, uint32_t data)
{
	switch (offset & 3)
	{
	case 0: vram_w(data);       break;
	case 1: command_w(data);    break;
	case 2: palette_w(data);    break;
	case 3: register_w(data);   break;
	}
}

UINT8 v99x8_device::vram_r()
{
	UINT8 ret;
	int address;

	address = ((int)m_cont_reg[14] << 14) | m_address_latch;

	m_cmd_write_first = 0;

	ret = m_read_ahead;

	if (m_cont_reg[45] & 0x40)  // Expansion memory
	{
		if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
			address >>= 1;  // correct?
		// Expansion memory only offers 64 K
		if (m_vram_size > 0x20000 && ((address & 0x10000)==0))
			m_read_ahead = m_vram_space->read_byte(address + EXPMEM_OFFSET);
		else
			m_read_ahead = 0xff;
	}
	else
	{
		m_read_ahead = vram_read(address);
	}

	m_address_latch = (m_address_latch + 1) & 0x3fff;
	if ((!m_address_latch) && (m_cont_reg[0] & 0x0c) ) // correct ???
	{
		m_cont_reg[14] = (m_cont_reg[14] + 1) & 7;
	}

	return ret;
}

UINT8 v99x8_device::status_r()
{
	int reg;
	UINT8 ret;

	m_cmd_write_first = 0;

	reg = m_cont_reg[15] & 0x0f;
	if (reg > 9)
		return 0xff;

	switch (reg)
	{
	case 0:
		ret = m_stat_reg[0];
		m_stat_reg[0] &= 0x1f;
		break;
	case 1:
		ret = m_stat_reg[1];
		m_stat_reg[1] &= 0xfe;
		if ((m_cont_reg[8] & 0xc0) == 0x80)
			// mouse mode: add button state
		ret |= m_button_state & 0xc0;
		break;
	case 2:
		/*update_command ();*/
		/*
		WTF is this? Whatever this was intended to do, it is nonsensical.
		Might as well pick a random number....
		This was an attempt to emulate H-Blank flag ;)
		n = cycles_currently_ran ();
		if ( (n < 28) || (n > 199) ) vdp.statReg[2] |= 0x20;
		else vdp.statReg[2] &= ~0x20;
		*/
//		if (machine().rand() & 1) m_stat_reg[2] |= 0x20;
//		else m_stat_reg[2] &= ~0x20;
		ret = m_stat_reg[2];
		break;
	case 3:
		if ((m_cont_reg[8] & 0xc0) == 0x80)
		{   // mouse mode: return x mouse delta
			ret = m_mx_delta;
			m_mx_delta = 0;
		}
		else
			ret = m_stat_reg[3];
		break;
	case 5:
		if ((m_cont_reg[8] & 0xc0) == 0x80)
		{   // mouse mode: return y mouse delta
			ret = m_my_delta;
			m_my_delta = 0;
		}
		else
			ret = m_stat_reg[5];
		break;
	case 7:
		ret = m_stat_reg[7];
		m_stat_reg[7] = m_cont_reg[44] = vdp_to_cpu () ;
		break;
	default:
		ret = m_stat_reg[reg];
		break;
	}

	LOG(("V9938: Read %02x from S#%d\n", ret, reg));
	check_int ();

	return ret;
}

void v99x8_device::palette_w(UINT8 data)
{
	int indexp;

	if (m_pal_write_first)
	{
		// store in register
		indexp = m_cont_reg[0x10] & 15;
		m_pal_reg[indexp*2] = m_pal_write & 0x77;
		m_pal_reg[indexp*2+1] = data & 0x07;
		// update palette
		m_pal_ind16[indexp] = (((int)m_pal_write << 2) & 0x01c0)  |
		(((int)data << 3) & 0x0038)  |
		((int)m_pal_write & 0x0007);

		m_cont_reg[0x10] = (m_cont_reg[0x10] + 1) & 15;
		m_pal_write_first = 0;
	}
	else
	{
		m_pal_write = data;
		m_pal_write_first = 1;
	}
}

void v99x8_device::vram_w(UINT8 data)
{
	int address;

	/*update_command ();*/

	m_cmd_write_first = 0;

	address = ((int)m_cont_reg[14] << 14) | m_address_latch;

	if (m_cont_reg[45] & 0x40)
	{
		if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
			address >>= 1;  // correct?
		if (m_vram_size > 0x20000 && ((address & 0x10000)==0))
			m_vram_space->write_byte(EXPMEM_OFFSET + address, data);
	}
	else
	{
		vram_write(address, data);
	}

	m_address_latch = (m_address_latch + 1) & 0x3fff;
	if ((!m_address_latch) && (m_cont_reg[0] & 0x0c) ) // correct ???
	{
		m_cont_reg[14] = (m_cont_reg[14] + 1) & 7;
	}
}

void v99x8_device::command_w(UINT8 data)
{
	if (m_cmd_write_first)
	{
		if (data & 0x80)
		{
			if (!(data & 0x40))
				register_write (data & 0x3f, m_cmd_write);
		}
		else
		{
			m_address_latch =
			(((UINT16)data << 8) | m_cmd_write) & 0x3fff;
			if ( !(data & 0x40) ) vram_r (); // read ahead!
		}

		m_cmd_write_first = 0;
	}
	else
	{
		m_cmd_write = data;
		m_cmd_write_first = 1;
	}
}

void v99x8_device::register_w(UINT8 data)
{
	int reg;

	reg = m_cont_reg[17] & 0x3f;
	if (reg != 17)
		register_write(reg, data); // true ?

	if (!(m_cont_reg[17] & 0x80))
		m_cont_reg[17] = (m_cont_reg[17] + 1) & 0x3f;
}

/*void v99x8_device::static_set_vram_size(device_t &device, UINT32 vram_size)
{
	downcast<v99x8_device &>(device).m_vram_size = vram_size;
}*/

/***************************************************************************

    Init/stop/reset/Interrupt functions

***************************************************************************/

void v99x8_device::device_start()
{
//	m_int_callback.resolve_safe();
	m_vdp_ops_count = 1;
	m_vdp_engine = NULL;

//	m_screen->register_screen_bitmap(m_bitmap);

	// Video RAM is allocated as an own address space
//	m_vram_space = &space(AS_DATA);

	// allocate VRAM
	assert(m_vram_size > 0);

	if (m_vram_size < 0x20000)
	{
		// set unavailable RAM to 0xff
		for (int addr = m_vram_size; addr < 0x30000; addr++) m_vram_space->write_byte(addr, 0xff);
	}

//	m_line_timer = timer_alloc(TIMER_LINE);

/*	save_item(NAME(m_offset_x));
	save_item(NAME(m_offset_y));
	save_item(NAME(m_visible_y));
	save_item(NAME(m_mode));
	save_item(NAME(m_pal_write_first));
	save_item(NAME(m_cmd_write_first));
	save_item(NAME(m_pal_write));
	save_item(NAME(m_cmd_write));
	save_item(NAME(m_pal_reg));
	save_item(NAME(m_stat_reg));
	save_item(NAME(m_cont_reg));
	save_item(NAME(m_read_ahead));
	//  save_item(NAME(m_vram));
	//  if ( m_vram_exp != NULL )
	//      save_pointer(NAME(m_vram_exp), 0x10000);
	save_item(NAME(m_int_state));
	save_item(NAME(m_scanline));
	save_item(NAME(m_blink));
	save_item(NAME(m_blink_count));
	save_item(NAME(m_mx_delta));
	save_item(NAME(m_my_delta));
	save_item(NAME(m_button_state));
	save_item(NAME(m_pal_ind16));
	save_item(NAME(m_pal_ind256));
	save_item(NAME(m_mmc.SX));
	save_item(NAME(m_mmc.SY));
	save_item(NAME(m_mmc.DX));
	save_item(NAME(m_mmc.DY));
	save_item(NAME(m_mmc.TX));
	save_item(NAME(m_mmc.TY));
	save_item(NAME(m_mmc.NX));
	save_item(NAME(m_mmc.NY));
	save_item(NAME(m_mmc.MX));
	save_item(NAME(m_mmc.ASX));
	save_item(NAME(m_mmc.ADX));
	save_item(NAME(m_mmc.ANX));
	save_item(NAME(m_mmc.CL));
	save_item(NAME(m_mmc.LO));
	save_item(NAME(m_mmc.CM));
	save_item(NAME(m_mmc.MXS));
	save_item(NAME(m_mmc.MXD));
	save_item(NAME(m_vdp_ops_count));
	save_item(NAME(m_pal_ntsc));
	save_item(NAME(m_scanline_start));
	save_item(NAME(m_vblank_start));
	save_item(NAME(m_scanline_max));
	save_item(NAME(m_height));*/
}

void v99x8_device::device_reset()
{
	int i;

	// offset reset
	m_offset_x = 8;
	m_offset_y = 0;
	m_visible_y = 192;
	// register reset
	reset_palette (); // palette registers
	for (i=0;i<10;i++) m_stat_reg[i] = 0;
	m_stat_reg[2] = 0x0c;
	if (m_model == MODEL_V9958) m_stat_reg[1] |= 4;
	for (i=0;i<48;i++) m_cont_reg[i] = 0;
	m_cmd_write_first = m_pal_write_first = 0;
	m_int_state = 0;
	m_read_ahead = 0; m_address_latch = 0; // ???
	m_scanline = 0;
	// MZ: The status registers 4 and 6 hold the high bits of the sprite
	// collision location. The unused bits are set to 1.
	// SR3: x x x x x x x x
	// SR4: 1 1 1 1 1 1 1 x
	// SR5: y y y y y y y y
	// SR6: 1 1 1 1 1 1 y y
	// Note that status register 4 is used in detection algorithms to tell
	// apart the tms9929 from the v99x8.

	// TODO: SR3-S6 do not yet store the information about the sprite collision
	m_stat_reg[4] = 0xfe;
	m_stat_reg[6] = 0xfc;

	// Start the timer
//	m_line_timer->adjust(attotime::from_ticks(HTOTAL*2, m_clock), 0, attotime::from_ticks(HTOTAL*2, m_clock));

	configure_pal_ntsc();
	set_screen_parameters();
}


void v99x8_device::reset_palette()
{
	// taken from V9938 Technical Data book, page 148. it's in G-R-B format
	static const UINT8 pal16[16*3] = {
		0, 0, 0, // 0: black/transparent
		0, 0, 0, // 1: black
		6, 1, 1, // 2: medium green
		7, 3, 3, // 3: light green
		1, 1, 7, // 4: dark blue
		3, 2, 7, // 5: light blue
		1, 5, 1, // 6: dark red
		6, 2, 7, // 7: cyan
		1, 7, 1, // 8: medium red
		3, 7, 3, // 9: light red
		6, 6, 1, // 10: dark yellow
		6, 6, 4, // 11: light yellow
		4, 1, 1, // 12: dark green
		2, 6, 5, // 13: magenta
		5, 5, 5, // 14: gray
		7, 7, 7  // 15: white
	};
	int i, red, ind;

	for (i=0;i<16;i++)
	{
		// set the palette registers
		m_pal_reg[i*2+0] = pal16[i*3+1] << 4 | pal16[i*3+2];
		m_pal_reg[i*2+1] = pal16[i*3];
		// set the reference table
		m_pal_ind16[i] = pal16[i*3+1] << 6 | pal16[i*3] << 3 | pal16[i*3+2];
	}

	// set internal palette GRAPHIC 7
	for (i=0;i<256;i++)
	{
		ind = (i << 4) & 0x01c0;
		ind |= (i >> 2) & 0x0038;
		red = (i << 1) & 6; if (red == 6) red++;
		ind |= red;

		m_pal_ind256[i] = ind;
	}
}

/***************************************************************************

Memory functions

***************************************************************************/

void v99x8_device::vram_write(int offset, int data)
{
	int newoffset;

	if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
	{
		newoffset = ((offset & 1) << 16) | (offset >> 1);
		if (newoffset < m_vram_size)
			m_vram_space->write_byte(newoffset, data);
	}
	else
	{
		if (offset < m_vram_size)
			m_vram_space->write_byte(offset, data);
	}
}

int v99x8_device::vram_read(int offset)
{
	if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
		return m_vram_space->read_byte(((offset & 1) << 16) | (offset >> 1));
	else
		return m_vram_space->read_byte(offset);
}

void v99x8_device::check_int()
{
	UINT8 n;

	n = ( (m_cont_reg[1] & 0x20) && (m_stat_reg[0] & 0x80) /*&& m_vblank_int*/) ||
	( (m_stat_reg[1] & 0x01) && (m_cont_reg[0] & 0x10) );

	#if 0
	if(n && m_vblank_int)
	{
		m_vblank_int = 0;
	}
	#endif

	if (n != m_int_state)
	{
		m_int_state = n;
		LOG(("V9938: IRQ line %s\n", n ? "up" : "down"));
	}

	/*
	** Somehow the IRQ request is going down without cpu_irq_line () being
	** called; because of this Mr. Ghost, Xevious and SD Snatcher don't
	** run. As a patch it's called every scanline
	*/
	if(!emu->now_waiting_in_debugger) {
//		m_int_callback(n);
		write_signals(&outputs_irq, n ? 0xffffffff : 0);
	}
}

/***************************************************************************

    Register functions

***************************************************************************/

void v99x8_device::register_write (int reg, int data)
{
	static UINT8 const reg_mask[] =
	{
		0x7e, 0x7b, 0x7f, 0xff, 0x3f, 0xff, 0x3f, 0xff,
		0xfb, 0xbf, 0x07, 0x03, 0xff, 0xff, 0x07, 0x0f,
		0x0f, 0xbf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x7f, 0x3f, 0x07
	};

	if (reg <= 27)
	{
		data &= reg_mask[reg];
		if (m_cont_reg[reg] == data)
			return;
	}

	if (reg > 46)
	{
		LOG(("V9938: Attempted to write to non-existant R#%d\n", reg));
		return;
	}

	/*update_command ();*/

	switch (reg) {
		// registers that affect interrupt and display mode
	case 0:
	case 1:
		m_cont_reg[reg] = data;
		set_mode();
		check_int();
		LOG(("v9938: mode = %s\n", v9938_modes[m_mode]));
		break;

	case 18:
	case 9:
		m_cont_reg[reg] = data;
		// recalc offset
		m_offset_x = 8 + position_offset(m_cont_reg[18] & 0x0f);
		// Y offset is only applied once per frame?
		break;

	case 15:
		m_pal_write_first = 0;
		break;

		// color burst registers aren't emulated
	case 20:
	case 21:
	case 22:
		LOG(("v9938: Write %02xh to R#%d; color burst not emulated\n", data, reg));
		break;
	case 25:
	case 26:
	case 27:
		if (m_model != MODEL_V9958)
		{
			LOG(("v9938: Attempting to write %02xh to V9958 R#%d\n", data, reg));
			data = 0;
		}
		else
		{
			if(reg == 25)
				m_v9958_sp_mode = data & 0x18;
		}
		break;

	case 44:
		cpu_to_vdp (data);
		break;

	case 46:
		command_unit_w (data);
		break;
	}

	if (reg != 15)
		LOG(("v9938: Write %02x to R#%d\n", data, reg));

	m_cont_reg[reg] = data;
}

/***************************************************************************

Refresh / render function

***************************************************************************/

inline bool v99x8_device::v9938_second_field()
{
	return !(((m_cont_reg[9] & 0x04) && !(m_stat_reg[2] & 2)) || m_blink);
}


void v99x8_device::default_border(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp)
{
	scrntype_t pen, tb;
	int i;

	pen = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	tb = GET_TP(m_cont_reg[7] & 0xf);
	i = LONG_WIDTH;
	while (i--) {
		*ln++ = pen;
		*tp++ = tb;
	}
}

void v99x8_device::graphic7_border(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp)
{
	scrntype_t pen, tb;
	int i;

	pen = pens[m_pal_ind256[m_cont_reg[7]]];
	tb = GET_TP(m_cont_reg[7] & 0xf);
	i = LONG_WIDTH;
	while (i--) {
		*ln++ = pen;
		*tp++ = tb;
	}
}

void v99x8_device::graphic5_border(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp)
{
	int i;
	scrntype_t pen0, tb0;
	scrntype_t pen1, tb1;

	pen1 = pens[m_pal_ind16[(m_cont_reg[7]&0x03)]];
	pen0 = pens[m_pal_ind16[((m_cont_reg[7]>>2)&0x03)]];
	tb0 = GET_TP((m_cont_reg[7] >> 2) & 0x03);
	tb1 = GET_TP(m_cont_reg[7] & 0x03);
	i = LONG_WIDTH / 2;
	while (i--) {
		*ln++ = pen0;
		*ln++ = pen1;
		*tp++ = tb0;
		*tp++ = tb1;
	}
}

void v99x8_device::mode_text1(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	int pattern, x, xx, name, xxx;
	scrntype_t fg, bg, pen, tf, tb;
	int nametbl_addr, patterntbl_addr;

	patterntbl_addr = m_cont_reg[4] << 11;
	nametbl_addr = m_cont_reg[2] << 10;

	fg = pens[m_pal_ind16[m_cont_reg[7] >> 4]];
	bg = pens[m_pal_ind16[m_cont_reg[7] & 15]];
	tf = GET_TP(m_cont_reg[7] >> 4);
	tb = GET_TP(m_cont_reg[7] & 15);
	name = (line/8)*40;

	pen = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	xxx = (m_offset_x + 8) * 2;
	while (xxx--) {
		*ln++ = pen;
		*tp++ = tb;
	}

	for (x=0;x<40;x++)
	{
		pattern = m_vram_space->read_byte(patterntbl_addr + (m_vram_space->read_byte(nametbl_addr + name) * 8) +
			((line + m_cont_reg[23]) & 7));
		for (xx=0;xx<6;xx++)
		{
			*ln++ = (pattern & 0x80) ? fg : bg;
			*ln++ = (pattern & 0x80) ? fg : bg;
			*tp++ = (pattern & 0x80) ? tf : tb;
			*tp++ = (pattern & 0x80) ? tf : tb;
			pattern <<= 1;
		}
		/* width height 212, characters start repeating at the bottom */
		name = (name + 1) & 0x3ff;
	}

	xxx = ((16 - m_offset_x) + 8) * 2;
	while (xxx--) {
		*ln++ = pen;
		*tp++ = tb;
	}
}

void v99x8_device::mode_text2(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	int pattern, x, charcode, name, xxx, patternmask, colourmask;
	scrntype_t fg, bg, fg0, bg0, pen, tf, tb, tf0, tb0;
	int nametbl_addr, patterntbl_addr, colourtbl_addr;

	patterntbl_addr = m_cont_reg[4] << 11;
	colourtbl_addr =  ((m_cont_reg[3] & 0xf8) << 6) + (m_cont_reg[10] << 14);
	#if 0
	colourmask = ((m_cont_reg[3] & 7) << 5) | 0x1f; /* cause a bug in Forth+ v1.0 on Geneve */
	#else
	colourmask = ((m_cont_reg[3] & 7) << 6) | 0x3f; /* verify! */
	#endif
	nametbl_addr = ((m_cont_reg[2] & 0xfc) << 10);
	patternmask = ((m_cont_reg[2] & 3) << 10) | 0x3ff; /* seems correct */

	fg = pens[m_pal_ind16[m_cont_reg[7] >> 4]];
	bg = pens[m_pal_ind16[m_cont_reg[7] & 15]];
	fg0 = pens[m_pal_ind16[m_cont_reg[12] >> 4]];
	bg0 = pens[m_pal_ind16[m_cont_reg[12] & 15]];
	tf = GET_TP(m_cont_reg[7] >> 4);
	tb = GET_TP(m_cont_reg[7] & 15);
	tf0 = GET_TP(m_cont_reg[12] >> 4);
	tb0 = GET_TP(m_cont_reg[12] & 15);
	name = (line/8)*80;

	xxx = (m_offset_x + 8) * 2;
	pen = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	while (xxx--) {
		*ln++ = pen;
		*tp++ = tb;
	}

	for (x=0;x<80;x++)
	{
		charcode = m_vram_space->read_byte(nametbl_addr + (name&patternmask));
		if (m_blink)
		{
			pattern = m_vram_space->read_byte(colourtbl_addr + ((name/8)&colourmask));
			if (pattern & (0x80 >> (name & 7) ) )
			{
				pattern = m_vram_space->read_byte(patterntbl_addr + ((charcode * 8) +
					((line + m_cont_reg[23]) & 7)));

				*ln++ = (pattern & 0x80) ? fg0 : bg0;
				*ln++ = (pattern & 0x40) ? fg0 : bg0;
				*ln++ = (pattern & 0x20) ? fg0 : bg0;
				*ln++ = (pattern & 0x10) ? fg0 : bg0;
				*ln++ = (pattern & 0x08) ? fg0 : bg0;
				*ln++ = (pattern & 0x04) ? fg0 : bg0;
				*tp++ = (pattern & 0x80) ? tf0 : tb0;
				*tp++ = (pattern & 0x40) ? tf0 : tb0;
				*tp++ = (pattern & 0x20) ? tf0 : tb0;
				*tp++ = (pattern & 0x10) ? tf0 : tb0;
				*tp++ = (pattern & 0x08) ? tf0 : tb0;
				*tp++ = (pattern & 0x04) ? tf0 : tb0;
				name++;
				continue;
			}
		}

		pattern = m_vram_space->read_byte(patterntbl_addr + ((charcode * 8) +
			((line + m_cont_reg[23]) & 7)));

		*ln++ = (pattern & 0x80) ? fg : bg;
		*ln++ = (pattern & 0x40) ? fg : bg;
		*ln++ = (pattern & 0x20) ? fg : bg;
		*ln++ = (pattern & 0x10) ? fg : bg;
		*ln++ = (pattern & 0x08) ? fg : bg;
		*ln++ = (pattern & 0x04) ? fg : bg;
		*tp++ = (pattern & 0x80) ? tf : tb;
		*tp++ = (pattern & 0x40) ? tf : tb;
		*tp++ = (pattern & 0x20) ? tf : tb;
		*tp++ = (pattern & 0x10) ? tf : tb;
		*tp++ = (pattern & 0x08) ? tf : tb;
		*tp++ = (pattern & 0x04) ? tf : tb;

		name++;
	}

	xxx = (16 - m_offset_x + 8) * 2;
	while (xxx--) {
		*ln++ = pen;
		*tp++ = tb;
	}
}

void v99x8_device::mode_multi(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	int nametbl_addr, patterntbl_addr, colour;
	int name, line2, x, xx;
	scrntype_t pen, pen_bg, tf, tb;

	nametbl_addr = (m_cont_reg[2] << 10);
	patterntbl_addr = (m_cont_reg[4] << 11);

	line2 = (line - m_cont_reg[23]) & 255;
	name = (line2/8)*32;

	pen_bg = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	tb = GET_TP(m_cont_reg[7] & 0xf);

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;
	xx = m_offset_x * 2 + masked;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tb;
	}

	int hoff_h = (m_model == MODEL_V9958) ? m_cont_reg[26] & 31 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? m_cont_reg[27] & 7 : 0;
	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}

	for (x = 0; x < 32; x++)
	{
		colour = m_vram_space->read_byte(patterntbl_addr + (m_vram_space->read_byte(nametbl_addr + name + hoff_h) * 8) + ((line2/4)&7));
		pen = pens[m_pal_ind16[colour>>4]];
		tf = GET_TP(colour >> 4);
		/* eight pixels */
		for (int i = 0; i < 8 && (x * 16 + i + hoff_l * 2) < 512; i++) {
			if (masked > 0 && (x * 16 + i) < (8 - hoff_l) * 2) {
				ln++;
				tp++;
			}
			else {
				*ln++ = pen;
				*tp++ = tf;
			}
		}
		pen = pens[m_pal_ind16[colour&15]];
		tf = GET_TP(colour & 15);
		/* eight pixels */
		for (int i = 0; i < 8 && (x * 16 + 8 + i + hoff_l * 2) < 512; i++) {
			if (masked > 0 && (x * 16 + 8 + i) < (8 - hoff_l) * 2) {
				ln++;
				tp++;
			}
			else {
				*ln++ = pen;
				*tp++ = tf;
			}
		}
		hoff_h = (hoff_h + 1) & 31;
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tb;
	}
}

void v99x8_device::mode_graphic1(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	scrntype_t fg, bg, pen, tf, tb, toff;
	int nametbl_addr, patterntbl_addr, colourtbl_addr;
	int pattern, x, xx, line2, name, charcode, colour, xxx;

	nametbl_addr = (m_cont_reg[2] << 10);
	colourtbl_addr = (m_cont_reg[3] << 6) + (m_cont_reg[10] << 14);
	patterntbl_addr = (m_cont_reg[4] << 11);

	line2 = (line - m_cont_reg[23]) & 255;

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;
	pen = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	toff = GET_TP(m_cont_reg[7] & 0x0f);
	xxx = m_offset_x * 2 + masked;
	while (xxx--) {
		*ln++ = pen;
		*tp++ = toff;
	}

	int hoff_h = (m_model == MODEL_V9958) ? m_cont_reg[26] & 31 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? m_cont_reg[27] & 7 : 0;
	name = (line2 / 8) * 32;
	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}
	for (x=0; x<32 ;x++)
	{
		charcode = m_vram_space->read_byte(nametbl_addr + name + hoff_h);
		colour = m_vram_space->read_byte(colourtbl_addr + charcode/8);
		fg = pens[m_pal_ind16[colour>>4]];
		bg = pens[m_pal_ind16[colour&15]];
		tf = GET_TP(colour >> 4);
		tb = GET_TP(colour & 15);
		pattern = m_vram_space->read_byte(patterntbl_addr + (charcode * 8 + (line2 & 7)));

		for (xx=0; xx<8; xx++)
		{
			if (x == 31 && (xx + hoff_l) > 7) {
				break;
			}
			if (x == 0 && xx < (8 - hoff_l) && masked > 0) {
				ln += 2;
				tp += 2;
			}
			else {
				*ln++ = (pattern & 0x80) ? fg : bg;
				*ln++ = (pattern & 0x80) ? fg : bg;
				*tp++ = (pattern & 0x80) ? tf : tb;
				*tp++ = (pattern & 0x80) ? tf : tb;
			}
			pattern <<= 1;
		}
		hoff_h = (hoff_h + 1) & 31;
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) {
		*ln++ = pen;
		*tp++ = toff;
	}
}

void v99x8_device::mode_graphic23(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	scrntype_t fg, bg, tf, tb, pen, toff;
	int nametbl_addr, patterntbl_addr, colourtbl_addr;
	int pattern, x, xx, line2, name, charcode,
	colour, colourmask, patternmask, xxx;

	colourmask = ((m_cont_reg[3] & 0x7f) * 8) | 7;
	patternmask = ((m_cont_reg[4] & 0x03) * 256) | 0xff;

	nametbl_addr =  (m_cont_reg[2] << 10);
	colourtbl_addr =  ((m_cont_reg[3] & 0x80) << 6) + (m_cont_reg[10] << 14);
	patterntbl_addr = ((m_cont_reg[4] & 0x3c) << 11);

	line2 = (line + m_cont_reg[23]) & 255;
	name = (line2/8)*32;

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;

	pen = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	toff = GET_TP(m_cont_reg[7] & 0x0f);
	xxx = m_offset_x * 2 + masked;
	while (xxx--) {
		*ln++ = pen;
		*tp++ = toff;
	}
	int hoff_h = (m_model == MODEL_V9958) ? m_cont_reg[26] & 31 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? m_cont_reg[27] & 7 : 0;
	name = (line2 / 8) * 32;
	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}

	for (x=0;x<32;x++)
	{
		charcode = m_vram_space->read_byte(nametbl_addr + name + hoff_h) + (line2&0xc0)*4;
		colour = m_vram_space->read_byte(colourtbl_addr + ((charcode&colourmask)*8+(line2&7)));
		pattern = m_vram_space->read_byte(patterntbl_addr + ((charcode&patternmask)*8+(line2&7)));
		fg = pens[m_pal_ind16[colour>>4]];
		bg = pens[m_pal_ind16[colour&15]];
		tf = GET_TP(colour >> 4);
		tb = GET_TP(colour & 15);
		for (xx=0;xx<8;xx++)
		{
			if (x == 31 && (xx + hoff_l) > 7) {
				break;
			}
			if (x == 0 && xx < (8 - hoff_l) && masked > 0) {
				ln += 2;
				tp += 2;
			}
			else {
				*ln++ = (pattern & 0x80) ? fg : bg;
				*ln++ = (pattern & 0x80) ? fg : bg;
				*tp++ = (pattern & 0x80) ? tf : tb;
				*tp++ = (pattern & 0x80) ? tf : tb;
			}
			pattern <<= 1;
		}
		hoff_h = (hoff_h + 1) & 31;
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) {
		*ln++ = pen;
		*tp++ = toff;
	}
}

void v99x8_device::mode_graphic4(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	int nametbl_addr, colour;
	int line2, linemask, x, xx;
	scrntype_t pen, pen_bg, tf, tp_bg;

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	pen_bg = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	tp_bg = GET_TP(m_cont_reg[7] & 0x0f);

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;
	xx = m_offset_x * 2 + masked;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tp_bg;
	}

	int hoff_h = (m_model == MODEL_V9958) ? (m_cont_reg[26] & 31) * 4 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? (m_cont_reg[27] & 7) : 0;
	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}
	nametbl_addr = ((m_cont_reg[2] & 0x40) << 10) + line2 * 128;
	if ((m_cont_reg[2] & 0x20) && v9938_second_field())
		nametbl_addr += 0x8000;

	for (x = 0; x < 128 && ((x * 2) + hoff_l) < 256; x++)
	{
		colour = m_vram_space->read_byte(nametbl_addr + hoff_h);
		if ((x * 2) < (8 - hoff_l) && masked > 0) {
			ln += 2;
			tp += 2;
		}
		else {
			pen = pens[m_pal_ind16[colour >> 4]];
			tf = GET_TP(colour >> 4);
			*ln++ = pen;
			*ln++ = pen;
			*tp++ = tf;
			*tp++ = tf;
		}
		if (((x * 2) + hoff_l + 1) < 256) {
			if ((x * 2 + 1) < (8 - hoff_l) && masked > 0) {
				ln += 2;
				tp += 2;
			}
			else {
				pen = pens[m_pal_ind16[colour & 15]];
				tf = GET_TP(colour & 15);
				*ln++ = pen;
				*ln++ = pen;
				*tp++ = tf;
				*tp++ = tf;
			}
		}
		hoff_h++;
		if (hoff_h >= 128) {
			hoff_h = 0;
			nametbl_addr = ((m_cont_reg[2] & 0x40) << 10) + line2 * 128;	//always page 0
		}
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tp_bg;
	}
}

void v99x8_device::mode_graphic5(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	int nametbl_addr, colour;
	int line2, linemask, x, xx;
	scrntype_t pen_bg0[4];
	scrntype_t pen_bg1[4];

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	nametbl_addr = ((m_cont_reg[2] & 0x40) << 10) + line2 * 128;
	if ( (m_cont_reg[2] & 0x20) && v9938_second_field() )
		nametbl_addr += 0x8000;

	pen_bg1[0] = pens[m_pal_ind16[(m_cont_reg[7]&0x03)]];
	pen_bg0[0] = pens[m_pal_ind16[((m_cont_reg[7]>>2)&0x03)]];

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;
	xx = m_offset_x + (masked / 2);
	while (xx--) { 
		*ln++ = pen_bg0[0];
		*ln++ = pen_bg1[0];
		*tp++ = GET_TP(0);
		*tp++ = GET_TP(0);
	}

	x = (m_cont_reg[8] & 0x20) ? 0 : 1;

	for (;x<4;x++)
	{
		pen_bg0[x] = pens[m_pal_ind16[x]];
		pen_bg1[x] = pens[m_pal_ind16[x]];
	}

	int hoff_h = (m_model == MODEL_V9958) ? (m_cont_reg[26] & 31) * 4 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? (m_cont_reg[27] & 7) : 0;

	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}

	for (x = 0; x < 128 && (x * 2 + hoff_l) < 256; x++)
	{
		colour = m_vram_space->read_byte(nametbl_addr + hoff_h);
		if ((x * 2) < (8 - hoff_l) && masked > 0) {
			ln += 2;
			tp += 2;
		}
		else {
			*ln++ = pen_bg0[colour >> 6];
			*ln++ = pen_bg1[(colour >> 4) & 3];
			*tp++ = GET_TP(colour >> 6);
			*tp++ = GET_TP((colour >> 4) & 3);
		}
		if (((x * 2 + 1) + hoff_l) < 256) {
			if ((x * 2 + 1) < (8 - hoff_l) && masked > 0) {
				ln += 2;
				tp += 2;
			}
			else {
				*ln++ = pen_bg0[(colour >> 2) & 3];
				*ln++ = pen_bg1[(colour & 3)];
				*tp++ = GET_TP((colour >> 2) & 3);
				*tp++ = GET_TP(colour & 3);
			}
		}
		hoff_h++;
		if (hoff_h >= 128) {
			hoff_h = 0;
			nametbl_addr = ((m_cont_reg[2] & 0x40) << 10) + line2 * 128;	//always page 0
		}
	}

	pen_bg1[0] = pens[m_pal_ind16[(m_cont_reg[7]&0x03)]];
	pen_bg0[0] = pens[m_pal_ind16[((m_cont_reg[7]>>2)&0x03)]];
	xx = 16 - m_offset_x;
	while (xx--) {
		*ln++ = pen_bg0[0];
		*ln++ = pen_bg1[0];
		*tp++ = GET_TP(0);
		*tp++ = GET_TP(0);
	}
}

void v99x8_device::mode_graphic6(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	UINT8 colour;
	int line2, linemask, x, xx, nametbl_addr;
	scrntype_t pen_bg, fg0;
	scrntype_t fg1, tf0, tf1, tb;

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	nametbl_addr = line2 << 8 ;
	if ( (m_cont_reg[2] & 0x20) && v9938_second_field() )
		nametbl_addr += 0x10000;

	pen_bg = pens[m_pal_ind16[(m_cont_reg[7]&0x0f)]];
	tb = GET_TP(m_cont_reg[7] & 0xf);

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;
	xx = m_offset_x * 2 + masked;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tb;
	}

	int hoff_h = (m_model == MODEL_V9958) ? (m_cont_reg[26] & 31) * 8 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? (m_cont_reg[27] & 7) : 0;

	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}

	if (m_model == MODEL_V9938 && m_cont_reg[2] & 0x40)
	{
		for (x=0;x<32;x++)
		{
			nametbl_addr++;
			colour = m_vram_space->read_byte(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			fg0 = pens[m_pal_ind16[colour>>4]];
			fg1 = pens[m_pal_ind16[colour&15]];
			tf0 = GET_TP(colour >> 4);
			tf1 = GET_TP(colour & 15);
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*tp++ = tf0; *tp++ = tf1; *tp++ = tf0; *tp++ = tf1;
			*tp++ = tf0; *tp++ = tf1; *tp++ = tf0; *tp++ = tf1;
			*tp++ = tf0; *tp++ = tf1; *tp++ = tf0; *tp++ = tf1;
			*tp++ = tf0; *tp++ = tf1; *tp++ = tf0; *tp++ = tf1;
			nametbl_addr += 7;
		}
	}
	else
	{
		for (x = 0; (x + hoff_l) < 256; x++)
		{
			int nametbl = nametbl_addr + hoff_h;
			colour = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			if (x < (8 - hoff_l) && masked > 0) {
				ln += 2;
				tp += 2;
			}
			else {
				*ln++ = pens[m_pal_ind16[colour >> 4]];
				*ln++ = pens[m_pal_ind16[colour & 15]];
				*tp++ = GET_TP(colour >> 4);
				*tp++ = GET_TP(colour & 15);
			}
			hoff_h++;
			if (hoff_h >= 256) {
				hoff_h = 0;
				nametbl_addr = line2 << 8;	//always page 0
			}
		}
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tb;
	}
}

void v99x8_device::mode_graphic7(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	UINT8 colour;
	int line2, linemask, x, xx, nametbl_addr;
	scrntype_t pen, pen_bg, tf, tb;

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	nametbl_addr = line2 << 8;
	if ( (m_cont_reg[2] & 0x20) && v9938_second_field() )
		nametbl_addr += 0x10000;

	pen_bg = pens[m_pal_ind256[m_cont_reg[7]]];
	tb = GET_TP(m_cont_reg[7]);

	int masked = (m_model == MODEL_V9958 && m_cont_reg[25] & 2) ? 16 : 0;
	xx = m_offset_x * 2 + masked;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tb;
	}

	int hoff_h = (m_model == MODEL_V9958) ? (m_cont_reg[26] & 31) * 8 : 0;
	int hoff_l = (m_model == MODEL_V9958) ? (m_cont_reg[27] & 7) : 0;

	if (masked > 0) {
		ln -= (8 - hoff_l) * 2;
		tp -= (8 - hoff_l) * 2;
	}
	else {
		ln += hoff_l * 2;
		tp += hoff_l * 2;
	}

	if ((m_v9958_sp_mode & 0x18) == 0x08) // v9958 screen 12, puzzle star title screen
	{
		for (x=0; x<64; x++)
		{
			int colour[4];
			int ind;
			int nametbl = nametbl_addr + hoff_h;
			colour[0] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			nametbl++;
			colour[1] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			nametbl++;
			colour[2] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			nametbl++;
			colour[3] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));

			ind = (colour[0] & 7) << 11 | (colour[1] & 7) << 14 |
			(colour[2] & 7) << 5 | (colour[3] & 7) << 8;
			for (int xx = 0; xx < 4 && (x * 4 + xx + hoff_l) < 256; xx++) {
				if (masked > 0 && (x * 4 + xx) < (8 - hoff_l)) {
					ln += 2;
					tp += 2;
				}
				else {
					int Y = (colour[xx] >> 3) & 31;
					*ln++ = pens[s_pal_indYJK[ind | Y]];
					*ln++ = *(ln - 1);
					*tp++ = GET_TP(ind | Y);
					*tp++ = *(tp - 1);
				}
			}
			hoff_h += 4;
			if (hoff_h >= 256) {
				hoff_h = 0;
				nametbl_addr = line2 << 8;
			}
		}
	}
	else if ((m_v9958_sp_mode & 0x18) == 0x18) // v9958 screen 10/11, puzzle star & sexy boom gameplay
	{
		for (x = 0; x < 64; x++)
		{
			int colour[4];
			int ind;
			int nametbl = nametbl_addr + hoff_h;

			colour[0] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			nametbl++;
			colour[1] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			nametbl++;
			colour[2] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));
			nametbl++;
			colour[3] = m_vram_space->read_byte(((nametbl &1) << 16) | (nametbl >>1));

			ind = (colour[0] & 7) << 11 | (colour[1] & 7) << 14 |
			(colour[2] & 7) << 5 | (colour[3] & 7) << 8;
			for (int xx = 0; xx < 4 && (x * 4 + xx + hoff_l) < 256; xx++) {
				if (masked > 0 && (x * 4 + xx) < (8 - hoff_l)) {
					ln += 2;
					tp += 2;
				}
				else {
					int Y = (colour[xx] >> 3) & 30;
					*ln++ = pens[colour[xx] & 8 ? m_pal_ind16[colour[xx] >> 4] : s_pal_indYJK[ind | ((colour[xx] >> 3) & 30)]];
					*ln++ = *(ln - 1);
					*tp++ = GET_TP(((colour[xx] & 8) ? (colour[xx] >> 4) : (ind | ((colour[xx] >> 3) & 30))));
					*tp++ = *(tp - 1);
				}
			}
			hoff_h += 4;
			if (hoff_h >= 256) {
				hoff_h = 0;
				nametbl_addr = line2 << 8;
			}
		}
	}
	else if (m_model == MODEL_V9938 && m_cont_reg[2] & 0x40)
	{// light pen feature
		for (x = 0;x < 32; x++)
		{
			nametbl_addr++;
			colour = m_vram_space->read_byte(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			pen = pens[m_pal_ind256[colour]];
			tf = GET_TP(colour);
			for (int i = 0; i < 16; i++) {
				*ln++ = pen;
				*tp++ = tf;
			}
			nametbl_addr++;
		}
	}
	else
	{
		for (x = 0; (x + hoff_l) < 256; x++)
		{
			if (masked > 0 && x < (8 - hoff_l)) {
				ln += 2;
				tp += 2;
			}
			else {
				int nametbl = nametbl_addr + hoff_h;
				colour = m_vram_space->read_byte(((nametbl & 1) << 16) | (nametbl >> 1));
				pen = pens[m_pal_ind256[colour]];
				tf = GET_TP(colour);
				*ln++ = pen;
				*ln++ = pen;
				*tp++ = tf;
				*tp++ = tf;
			}
			hoff_h++;
			if (hoff_h >= 256) {
				hoff_h = 0;
				nametbl_addr = line2 << 8;
			}
		}
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) {
		*ln++ = pen_bg;
		*tp++ = tb;
	}
}

void v99x8_device::mode_unknown(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, int line)
{
	scrntype_t fg, bg, tf, tb;
	int x;

	fg = pens[m_pal_ind16[m_cont_reg[7] >> 4]];
	bg = pens[m_pal_ind16[m_cont_reg[7] & 15]];
	tf = GET_TP(m_cont_reg[7] >> 4);
	tb = GET_TP(m_cont_reg[7] & 15);

	x = m_offset_x * 2 + ((m_cont_reg[25] & 2) ? 16 : 0);
	while (x--) {
		*ln++ = bg;
		*tp++ = tb;
	}

	x = 512;
	while (x--) {
		*ln++ = fg;
		*tp++ = tf;
	}

	x = (16 - m_offset_x) * 2;
	while (x--) {
		*ln++ = bg;
		*tp++ = tf;
	}
}

void v99x8_device::default_draw_sprite(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, UINT8 *col)
{
	int i;
	ln += m_offset_x * 2;

	for (i=0;i<256;i++)
	{
		if (col[i] & 0x80)
		{
			*ln++ = pens[m_pal_ind16[col[i]&0x0f]];
			*ln++ = pens[m_pal_ind16[col[i]&0x0f]];
			*tp++ = (-1);
			*tp++ = (-1);
		}
		else
		{
			ln += 2;
			tp += 2;
		}
	}
}

void v99x8_device::graphic5_draw_sprite(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, UINT8 *col)
{
	int i;
	ln += m_offset_x * 2;

	for (i=0;i<256;i++)
	{
		if (col[i] & 0x80)
		{
			*ln++ = pens[m_pal_ind16[(col[i]>>2)&0x03]];
			*ln++ = pens[m_pal_ind16[col[i]&0x03]];
			*tp++ = (-1);
			*tp++ = (-1);
		}
		else
		{
			ln += 2;
			tp += 2;
		}
	}
}


void v99x8_device::graphic7_draw_sprite(const scrntype_t *pens, scrntype_t *ln, scrntype_t* tp, UINT8 *col)
{
	static const UINT16 g7_ind16[16] = {
		0, 2, 192, 194, 48, 50, 240, 242,
	482, 7, 448, 455, 56, 63, 504, 511  };
	int i;

	ln += m_offset_x * 2;

	for (i=0;i<256;i++)
	{
		if (col[i] & 0x80)
		{
			*ln++ = pens[g7_ind16[col[i]&0x0f]];
			*ln++ = pens[g7_ind16[col[i]&0x0f]];
			*tp++ = (-1);
			*tp++ = (-1);
		}
		else
		{
			ln += 2;
			tp += 2;
		}
	}
}


void v99x8_device::sprite_mode1 (int line, UINT8 *col)
{
	int attrtbl_addr, patterntbl_addr, pattern_addr;
	int x, y, p, height, c, p2, i, n, pattern;

	memset(col, 0, 256);

	// are sprites disabled?
	if (m_cont_reg[8] & 0x02) return;

	attrtbl_addr = (m_cont_reg[5] << 7) + (m_cont_reg[11] << 15);
	patterntbl_addr = (m_cont_reg[6] << 11);

	// 16x16 or 8x8 sprites
	height = (m_cont_reg[1] & 2) ? 16 : 8;
	// magnified sprites (zoomed)
	if (m_cont_reg[1] & 1) height *= 2;

	p2 = p = 0;
	while (1)
	{
		y = m_vram_space->read_byte(attrtbl_addr);
		if (y == 208) break;
		y = (y - m_cont_reg[23]) & 255;
		if (y > 208)
			y = -(~y&255);
		else
			y++;

		// if sprite in range, has to be drawn
		if ( (line >= y) && (line  < (y + height) ) )
		{
			if (p2 == 4)
			{
				// max maximum sprites per line!
				if ( !(m_stat_reg[0] & 0x40) )
					m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | 0x40 | p;

				break;
			}
			// get x
			x = m_vram_space->read_byte(attrtbl_addr + 1);
			if (m_vram_space->read_byte(attrtbl_addr + 3) & 0x80) x -= 32;

			// get pattern
			pattern = m_vram_space->read_byte(attrtbl_addr + 2);
			if (m_cont_reg[1] & 2)
				pattern &= 0xfc;
			n = line - y;
			pattern_addr = patterntbl_addr + pattern * 8 + ((m_cont_reg[1] & 1) ? n/2  : n);
			pattern = (m_vram_space->read_byte(pattern_addr) << 8) | m_vram_space->read_byte(pattern_addr+16);

			// get colour
			c = m_vram_space->read_byte(attrtbl_addr + 3) & 0x0f;

			// draw left part
			n = 0;
			while (1)
			{
				if (n == 0) pattern = m_vram_space->read_byte(pattern_addr);
				else if ( (n == 1) && (m_cont_reg[1] & 2) ) pattern = m_vram_space->read_byte(pattern_addr + 16);
				else break;

				n++;

				for (i=0;i<8;i++)
				{
					if (pattern & 0x80)
					{
						if ( (x >= 0) && (x < 256) )
						{
							if (col[x] & 0x40)
							{
								// we have a collision!
								if (p2 < 4)
									m_stat_reg[0] |= 0x20;
							}
							if ( !(col[x] & 0x80) )
							{
								if (c || (m_cont_reg[8] & 0x20) )
									col[x] |= 0xc0 | c;
								else
									col[x] |= 0x40;
							}

							// if zoomed, draw another pixel
							if (m_cont_reg[1] & 1)
							{
								if (col[x+1] & 0x40)
								{
									// we have a collision!
									if (p2 < 4)
										m_stat_reg[0] |= 0x20;
								}
								if ( !(col[x+1] & 0x80) )
								{
									if (c || (m_cont_reg[8] & 0x20) )
										col[x+1] |= 0xc0 | c;
									else
										col[x+1] |= 0x80;
								}
							}
						}
					}
					if (m_cont_reg[1] & 1) x += 2; else x++;
					pattern <<= 1;
				}
			}

			p2++;
		}

		if (p >= 31) break;
		p++;
		attrtbl_addr += 4;
	}

	if ( !(m_stat_reg[0] & 0x40) )
		m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | p;
}

void v99x8_device::sprite_mode2 (int line, UINT8 *col)
{
	int attrtbl_addr, patterntbl_addr, pattern_addr, colourtbl_addr;
	int x, i, y, p, height, c, p2, n, pattern, colourmask, first_cc_seen;

	memset(col, 0, 256);

	// are sprites disabled?
	if (m_cont_reg[8] & 0x02) return;

	attrtbl_addr = ( (m_cont_reg[5] & 0xfc) << 7) + (m_cont_reg[11] << 15);
	colourtbl_addr =  ( (m_cont_reg[5] & 0xf8) << 7) + (m_cont_reg[11] << 15);
	patterntbl_addr = (m_cont_reg[6] << 11);
	colourmask = ( (m_cont_reg[5] & 3) << 3) | 0x7; // check this!

	// 16x16 or 8x8 sprites
	height = (m_cont_reg[1] & 2) ? 16 : 8;
	// magnified sprites (zoomed)
	if (m_cont_reg[1] & 1) height *= 2;

	p2 = p = first_cc_seen = 0;
	while (1)
	{
		y = vram_read(attrtbl_addr);
		if (y == 216) break;
		y = (y - m_cont_reg[23]) & 255;
		if (y > 216)
			y = -(~y&255);
		else
			y++;

		// if sprite in range, has to be drawn
		if ( (line >= y) && (line  < (y + height) ) )
		{
			if (p2 == 8)
			{
				// max maximum sprites per line!
				if ( !(m_stat_reg[0] & 0x40) )
					m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | 0x40 | p;

				break;
			}

			n = line - y; if (m_cont_reg[1] & 1) n /= 2;
			// get colour
			c = vram_read(colourtbl_addr + (((p&colourmask)*16) + n));

			// don't draw all sprite with CC set before any sprites
			// with CC = 0 are seen on this line
			if (c & 0x40)
			{
				if (!first_cc_seen)
					goto skip_first_cc_set;
			}
			else
				first_cc_seen = 1;

			// get pattern
			pattern = vram_read(attrtbl_addr + 2);
			if (m_cont_reg[1] & 2)
				pattern &= 0xfc;
			pattern_addr = patterntbl_addr + pattern * 8 + n;
			pattern = (vram_read(pattern_addr) << 8) | vram_read(pattern_addr + 16);

			// get x
			x = vram_read(attrtbl_addr + 1);
			if (c & 0x80) x -= 32;

			n = (m_cont_reg[1] & 2) ? 16 : 8;
			while (n--)
			{
				for (i=0;i<=(m_cont_reg[1] & 1);i++)
				{
					if ( (x >= 0) && (x < 256) )
					{
						if ( (pattern & 0x8000) && !(col[x] & 0x10) )
						{
							if ( (c & 15) || (m_cont_reg[8] & 0x20) )
							{
								if ( !(c & 0x40) )
								{
									if (col[x] & 0x20) col[x] |= 0x10;
									else
										col[x] |= 0x20 | (c & 15);
								}
								else
									col[x] |= c & 15;

								col[x] |= 0x80;
							}
						}
						else
						{
							if ( !(c & 0x40) && (col[x] & 0x20) )
								col[x] |= 0x10;
						}

						if ( !(c & 0x60) && (pattern & 0x8000) )
						{
							if (col[x] & 0x40)
							{
								// sprite collision!
								if (p2 < 8)
									m_stat_reg[0] |= 0x20;
							}
							else
								col[x] |= 0x40;
						}

						x++;
					}
				}

				pattern <<= 1;
			}

		skip_first_cc_set:
			p2++;
		}

		if (p >= 31) break;
		p++;
		attrtbl_addr += 4;
	}

	if ( !(m_stat_reg[0] & 0x40) )
		m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | p;
}


const v99x8_device::v99x8_mode v99x8_device::s_modes[] = {
	{ 0x02,
		&v99x8_device::mode_text1,
		&v99x8_device::default_border,
		NULL,
		NULL
	},
	{ 0x01,
		&v99x8_device::mode_multi,
		&v99x8_device::default_border,
		&v99x8_device::sprite_mode1,
		&v99x8_device::default_draw_sprite
	},
	{ 0x00,
		&v99x8_device::mode_graphic1,
		&v99x8_device::default_border,
		&v99x8_device::sprite_mode1,
		&v99x8_device::default_draw_sprite
	},
	{ 0x04,
		&v99x8_device::mode_graphic23,
		&v99x8_device::default_border,
		&v99x8_device::sprite_mode1,
		&v99x8_device::default_draw_sprite
	},
	{ 0x08,
		&v99x8_device::mode_graphic23,
		&v99x8_device::default_border,
		&v99x8_device::sprite_mode2,
		&v99x8_device::default_draw_sprite
	},
	{ 0x0c,
		&v99x8_device::mode_graphic4,
		&v99x8_device::default_border,
		&v99x8_device::sprite_mode2,
		&v99x8_device::default_draw_sprite
	},
	{ 0x10,
		&v99x8_device::mode_graphic5,
		&v99x8_device::graphic5_border,
		&v99x8_device::sprite_mode2,
		&v99x8_device::graphic5_draw_sprite
	},
	{ 0x14,
		&v99x8_device::mode_graphic6,
		&v99x8_device::default_border,
		&v99x8_device::sprite_mode2,
		&v99x8_device::default_draw_sprite
	},
	{ 0x1c,
		&v99x8_device::mode_graphic7,
		&v99x8_device::graphic7_border,
		&v99x8_device::sprite_mode2,
		&v99x8_device::graphic7_draw_sprite
	},
	{ 0x0a,
		&v99x8_device::mode_text2,
		&v99x8_device::default_border,
		NULL,
		NULL
	},
	{ 0xff,
		&v99x8_device::mode_unknown,
		&v99x8_device::default_border,
		NULL,
		NULL
	}
};

void v99x8_device::set_mode()
{
	int n,i;

	n = (((m_cont_reg[0] & 0x0e) << 1) | ((m_cont_reg[1] & 0x18) >> 3));
	for (i=0;;i++)
	{
		if ( (s_modes[i].m == n) || (s_modes[i].m == 0xff) ) break;
	}
	m_mode = i;
}

void v99x8_device::refresh_16(int line)
{
	//const pen_t *pens = m_palette->pens();
	const scrntype_t *pens = this->pens;
	bool double_lines = false;
	UINT8 col[256];
	scrntype_t *ln, *ln2 = NULL;
	scrntype_t *tp, *tp2 = NULL;

	if (m_cont_reg[9] & 0x08)
	{
//		ln = &m_bitmap.pix16(m_scanline*2+((m_stat_reg[2]>>1)&1));
		ln = screen+(m_scanline*2+((m_stat_reg[2]>>1)&1))*LONG_WIDTH;
		tp = transparent + (m_scanline * 2 + ((m_stat_reg[2] >> 1) & 1)) * LONG_WIDTH;
	}
	else
	{
//		ln = &m_bitmap.pix16(m_scanline*2);
//		ln2 = &m_bitmap.pix16(m_scanline*2+1);
		ln = screen+(m_scanline*2)*LONG_WIDTH;
		ln2 = screen+(m_scanline*2+1)*LONG_WIDTH;
		tp = transparent + (m_scanline * 2) * LONG_WIDTH;
		tp2 = transparent + (m_scanline * 2 + 1) * LONG_WIDTH;
		double_lines = true;
	}

	if ( !(m_cont_reg[1] & 0x40) || (m_stat_reg[2] & 0x40) )
	{
		(this->*s_modes[m_mode].border_16)(pens, ln, tp);
	}
	else
	{
		(this->*s_modes[m_mode].visible_16)(pens, ln, tp, line);
		if (s_modes[m_mode].sprites)
		{
			(this->*s_modes[m_mode].sprites)(line, col);
			(this->*s_modes[m_mode].draw_sprite_16)(pens, ln, tp, col);
		}
	}

	if (double_lines) {
		my_memcpy(ln2, ln, (512 + 32) * sizeof(scrntype_t));
		my_memcpy(tp2, tp, (512 + 32) * sizeof(scrntype_t));
	}
}

void v99x8_device::refresh_line(int line)
{
	int ind16, ind256;

	ind16 = m_pal_ind16[0];
	ind256 = m_pal_ind256[0];

	if ( !(m_cont_reg[8] & 0x20) && (m_mode != V9938_MODE_GRAPHIC5) )
	{
		m_pal_ind16[0] = m_pal_ind16[(m_cont_reg[7] & 0x0f)];
		m_pal_ind256[0] = m_pal_ind256[m_cont_reg[7]];
	}

	refresh_16 (line);

	if ( !(m_cont_reg[8] & 0x20) && (m_mode != V9938_MODE_GRAPHIC5) )
	{
		m_pal_ind16[0] = ind16;
		m_pal_ind256[0] = ind256;
	}
}

/*

From: awulms@inter.nl.net (Alex Wulms)
*** About the HR/VR topic: this is how it works according to me:

*** HR:
HR is very straightforward:
-HR=1 during 'display time'
-HR=0 during 'horizontal border, horizontal retrace'
I have put 'display time' and 'horizontal border, horizontal retrace' between
quotes because HR does not only flip between 0 and 1 during the display of
the 192/212 display lines, but also during the vertical border and during the
vertical retrace.

*** VR:
VR is a little bit tricky
-VR always gets set to 0 when the VDP starts with display line 0
-VR gets set to 1 when the VDP reaches display line (192 if LN=0) or (212 if
LN=1)
-The VDP displays contents of VRAM as long as VR=0

As a consequence of this behaviour, it is possible to program the famous
overscan trick, where VRAM contents is shown in the borders:
Generate an interrupt at line 230 (or so) and on this interrupt: set LN=1
Generate an interrupt at line 200 (or so) and on this interrupt: set LN=0
Repeat the above two steps

*** The top/bottom border contents during overscan:
On screen 0:
1) The VDP keeps increasing the name table address pointer during bottom
border, vertical retrace and top border
2) The VDP resets the name table address pointer when the first display line
is reached

On the other screens:
1) The VDP keeps increasing the name table address pointer during the bottom
border
2) The VDP resets the name table address pointer such that the top border
contents connects up with the first display line. E.g., when the top border
is 26 lines high, the VDP will take:
'logical'      vram line
TOPB000  256-26
...
TOPB025  256-01
DISPL000 000
...
DISPL211 211
BOTB000  212
...
BOTB024  236



*** About the horizontal interrupt

All relevant definitions on a row:
-FH: Bit 0 of status register 1
-IE1: Bit 4 of mode register 0
-IL: Line number in mode register 19
-DL: The line that the VDP is going to display (corrected for vertical scroll)
-IRQ: Interrupt request line of VDP to Z80

At the *start* of every new line (display, bottom border, part of vertical
display), the VDP does:
-FH = (FH && IE1) || (IL==DL)

After reading of status register 1 by the CPU, the VDP does:
-FH = 0

Furthermore, the following is true all the time:
-IRQ = FH && IE1

The resulting behaviour:
When IE1=0:
-FH will be set as soon as display of line IL starts
-FH will be reset as soon as status register 1 is read
-FH will be reset as soon as the next display line is reached

When IE=1:
-FH and IRQ will be set as soon as display line IL is reached
-FH and IRQ will be reset as soon as status register 1 is read

Another subtile result:
If, while FH and IRQ are set, IE1 gets reset, the next happens:
-IRQ is reset immediately (since IRQ is always FH && IE1)
-FH will be reset as soon as display of the next line starts (unless the next
line is line IL)


*** About the vertical interrupt:
Another relevant definition:
-FV: Bit 7 of status register 0
-IE0: Bit 5 of mode register 1

I only know for sure the behaviour when IE0=1:
-FV and IRQ will be set as soon as VR changes from 0 to 1
-FV and IRQ will be reset as soon as status register 0 is read

A consequence is that NO vertical interrupts will be generated during the
overscan trick, described in the VR section above.

I do not know the behaviour of FV when IE0=0. That is the part that I still
have to test.
*/

void v99x8_device::interrupt_start_vblank()
{
	#if 0
	if (machine.input().code_pressed (KEYCODE_D) )
	{
		for (i=0;i<24;i++) osd_printf_debug ("R#%d = %02x\n", i, m_cont_reg[i]);
	}
	#endif

	// at every frame, vdp switches fields
	m_stat_reg[2] = (m_stat_reg[2] & 0xfd) | (~m_stat_reg[2] & 2);

	// color blinking
	if (!(m_cont_reg[13] & 0xf0))
		m_blink = 0;
	else if (!(m_cont_reg[13] & 0x0f))
		m_blink = 1;
	else
	{
		// both on and off counter are non-zero: timed blinking
		if (m_blink_count)
			m_blink_count--;
		if (!m_blink_count)
		{
			m_blink = !m_blink;
			if (m_blink)
				m_blink_count = (m_cont_reg[13] >> 4) * 10;
			else
				m_blink_count = (m_cont_reg[13] & 0x0f) * 10;
		}
	}
}

/***************************************************************************

Command unit

***************************************************************************/

/*************************************************************/
/** Completely rewritten by Alex Wulms:                     **/
/**  - VDP Command execution 'in parallel' with CPU         **/
/**  - Corrected behaviour of VDP commands                  **/
/**  - Made it easier to implement correct S7/8 mapping     **/
/**    by concentrating VRAM access in one single place     **/
/**  - Made use of the 'in parallel' VDP command exec       **/
/**    and correct timing. You must call the function       **/
/**    LoopVDP() from LoopZ80 in MSX.c. You must call it    **/
/**    exactly 256 times per screen refresh.                **/
/** Started on       : 11-11-1999                           **/
/** Beta release 1 on:  9-12-1999                           **/
/** Beta release 2 on: 20-01-2000                           **/
/**  - Corrected behaviour of VRM <-> Z80 transfer          **/
/**  - Improved performance of the code                     **/
/** Public release 1.0: 20-04-2000                          **/
/*************************************************************/

#define VDP_VRMP5(MX, X, Y) ((!MX) ? (((Y&1023)<<7) + ((X&255)>>1)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X&255)>>1)))
#define VDP_VRMP6(MX, X, Y) ((!MX) ? (((Y&1023)<<7) + ((X&511)>>2)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X&511)>>2)))
//#define VDP_VRMP7(MX, X, Y) ((!MX) ? (((Y&511)<<8) + ((X&511)>>1)) : (EXPMEM_OFFSET + ((Y&255)<<8) + ((X&511)>>1)))
#define VDP_VRMP7(MX, X, Y) ((!MX) ? (((X&2)<<15) + ((Y&511)<<7) + ((X&511)>>2)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X&511)>>2))/*(EXPMEM_OFFSET + ((Y&255)<<8) + ((X&511)>>1))*/)
//#define VDP_VRMP8(MX, X, Y) ((!MX) ? (((Y&511)<<8) + (X&255)) : (EXPMEM_OFFSET + ((Y&255)<<8) + (X&255)))
#define VDP_VRMP8(MX, X, Y) ((!MX) ? (((X&1)<<16) + ((Y&511)<<7) + ((X>>1)&127)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X>>1)&127))/*(EXPMEM_OFFSET + ((Y&255)<<8) + (X&255))*/)

#define VDP_VRMP(M, MX, X, Y) VDPVRMP(M, MX, X, Y)
#define VDP_POINT(M, MX, X, Y) VDPpoint(M, MX, X, Y)
#define VDP_PSET(M, MX, X, Y, C, O) VDPpset(M, MX, X, Y, C, O)

#define CM_ABRT  0x0
#define CM_POINT 0x4
#define CM_PSET  0x5
#define CM_SRCH  0x6
#define CM_LINE  0x7
#define CM_LMMV  0x8
#define CM_LMMM  0x9
#define CM_LMCM  0xA
#define CM_LMMC  0xB
#define CM_HMMV  0xC
#define CM_HMMM  0xD
#define CM_YMMM  0xE
#define CM_HMMC  0xF

/*************************************************************
Many VDP commands are executed in some kind of loop but
essentially, there are only a few basic loop structures
that are re-used. We define the loop structures that are
re-used here so that they have to be entered only once
*************************************************************/
#define pre_loop \
while ((cnt-=delta) > 0) {
	#define post_loop \
}

// Loop over DX, DY
#define post__x_y(MX) \
if (!--ANX || ((ADX+=TX)&MX)) { \
	if (!(--NY&1023) || (DY+=TY)==-1) \
		break; \
	else { \
		ADX=DX; \
		ANX=NX; \
	} \
} \
post_loop

// Loop over DX, SY, DY
#define post__xyy(MX) \
if ((ADX+=TX)&MX) { \
	if (!(--NY&1023) || (SY+=TY)==-1 || (DY+=TY)==-1) \
		break; \
	else \
		ADX=DX; \
} \
post_loop

// Loop over SX, DX, SY, DY
#define post_xxyy(MX) \
if (!--ANX || ((ASX+=TX)&MX) || ((ADX+=TX)&MX)) { \
	if (!(--NY&1023) || (SY+=TY)==-1 || (DY+=TY)==-1) \
		break; \
	else { \
		ASX=SX; \
		ADX=DX; \
		ANX=NX; \
	} \
} \
post_loop

/*************************************************************/
/** Variables visible only in this module                   **/
/*************************************************************/
static const UINT8 Mask[4] = { 0x0F,0x03,0x0F,0xFF };
static const int  PPB[4]  = { 2,4,2,1 };
static const int  PPL[4]  = { 256,512,512,256 };

//  SprOn SprOn SprOf SprOf
//  ScrOf ScrOn ScrOf ScrOn
static const int srch_timing[8]={
	818, 1025,  818,  830, // ntsc
	696,  854,  696,  684  // pal
};
static const int line_timing[8]={
	1063, 1259, 1063, 1161,
	904,  1026, 904,  953
};
static const int hmmv_timing[8]={
	439,  549,  439,  531,
	366,  439,  366,  427
};
static const int lmmv_timing[8]={
	873,  1135, 873, 1056,
	732,  909,  732,  854
};
static const int ymmm_timing[8]={
	586,  952,  586,  610,
	488,  720,  488,  500
};
static const int hmmm_timing[8]={
	818,  1111, 818,  854,
	684,  879,  684,  708
};
static const int lmmm_timing[8]={
	1160, 1599, 1160, 1172,
	964,  1257, 964,  977
};

/** VDPVRMP() **********************************************/
/** Calculate addr of a pixel in vram                       **/
/*************************************************************/
inline int v99x8_device::VDPVRMP(UINT8 M,int MX,int X,int Y)
{
	switch(M)
	{
	case 0: return VDP_VRMP5(MX,X,Y);
	case 1: return VDP_VRMP6(MX,X,Y);
	case 2: return VDP_VRMP7(MX,X,Y);
	case 3: return VDP_VRMP8(MX,X,Y);
	}

	return 0;
}

/** VDPpoint5() ***********************************************/
/** Get a pixel on screen 5                                 **/
/*************************************************************/
inline UINT8 v99x8_device::VDPpoint5(int MXS, int SX, int SY)
{
	return (m_vram_space->read_byte(VDP_VRMP5(MXS, SX, SY)) >>
		(((~SX)&1)<<2)
		)&15;
}

/** VDPpoint6() ***********************************************/
/** Get a pixel on screen 6                                 **/
/*************************************************************/
inline UINT8 v99x8_device::VDPpoint6(int MXS, int SX, int SY)
{
	return (m_vram_space->read_byte(VDP_VRMP6(MXS, SX, SY)) >>
		(((~SX)&3)<<1)
		)&3;
}

/** VDPpoint7() ***********************************************/
/** Get a pixel on screen 7                                 **/
/*************************************************************/
inline UINT8 v99x8_device::VDPpoint7(int MXS, int SX, int SY)
{
	return (m_vram_space->read_byte(VDP_VRMP7(MXS, SX, SY)) >>
		(((~SX)&1)<<2)
		)&15;
}

/** VDPpoint8() ***********************************************/
/** Get a pixel on screen 8                                 **/
/*************************************************************/
inline UINT8 v99x8_device::VDPpoint8(int MXS, int SX, int SY)
{
	return m_vram_space->read_byte(VDP_VRMP8(MXS, SX, SY));
}

/** VDPpoint() ************************************************/
/** Get a pixel on a screen                                 **/
/*************************************************************/
inline UINT8 v99x8_device::VDPpoint(UINT8 SM, int MXS, int SX, int SY)
{
	switch(SM)
	{
	case 0: return VDPpoint5(MXS,SX,SY);
	case 1: return VDPpoint6(MXS,SX,SY);
	case 2: return VDPpoint7(MXS,SX,SY);
	case 3: return VDPpoint8(MXS,SX,SY);
	}

	return(0);
}

/** VDPpsetlowlevel() ****************************************/
/** Low level function to set a pixel on a screen           **/
/** Make it inline to make it fast                          **/
/*************************************************************/
inline void v99x8_device::VDPpsetlowlevel(int addr, UINT8 CL, UINT8 M, UINT8 OP)
{
	// If this turns out to be too slow, get a pointer to the address space
	// and work directly on it.
	UINT8 val = m_vram_space->read_byte(addr);
	switch (OP)
	{
	case 0: val = (val & M) | CL; break;
	case 1: val = val & (CL | M); break;
	case 2: val |= CL; break;
	case 3: val ^= CL; break;
	case 4: val = (val & M) | ~(CL | M); break;
	case 8: if (CL) val = (val & M) | CL; break;
	case 9: if (CL) val = val & (CL | M); break;
	case 10: if (CL) val |= CL; break;
	case 11:  if (CL) val ^= CL; break;
	case 12:  if (CL) val = (val & M) | ~(CL|M); break;
	default:
		LOG(("v9938: invalid operation %d in pset\n", OP));
	}

	m_vram_space->write_byte(addr, val);
}

/** VDPpset5() ***********************************************/
/** Set a pixel on screen 5                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset5(int MXD, int DX, int DY, UINT8 CL, UINT8 OP)
{
	UINT8 SH = ((~DX)&1)<<2;
	VDPpsetlowlevel(VDP_VRMP5(MXD, DX, DY), CL << SH, ~(15<<SH), OP);
}

/** VDPpset6() ***********************************************/
/** Set a pixel on screen 6                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset6(int MXD, int DX, int DY, UINT8 CL, UINT8 OP)
{
	UINT8 SH = ((~DX)&3)<<1;

	VDPpsetlowlevel(VDP_VRMP6(MXD, DX, DY), CL << SH, ~(3<<SH), OP);
}

/** VDPpset7() ***********************************************/
/** Set a pixel on screen 7                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset7(int MXD, int DX, int DY, UINT8 CL, UINT8 OP)
{
	UINT8 SH = ((~DX)&1)<<2;

	VDPpsetlowlevel(VDP_VRMP7(MXD, DX, DY), CL << SH, ~(15<<SH), OP);
}

/** VDPpset8() ***********************************************/
/** Set a pixel on screen 8                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset8(int MXD, int DX, int DY, UINT8 CL, UINT8 OP)
{
	VDPpsetlowlevel(VDP_VRMP8(MXD, DX, DY), CL, 0, OP);
}

/** VDPpset() ************************************************/
/** Set a pixel on a screen                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset(UINT8 SM, int MXD, int DX, int DY, UINT8 CL, UINT8 OP)
{
	switch (SM) {
	case 0: VDPpset5(MXD, DX, DY, CL, OP); break;
	case 1: VDPpset6(MXD, DX, DY, CL, OP); break;
	case 2: VDPpset7(MXD, DX, DY, CL, OP); break;
	case 3: VDPpset8(MXD, DX, DY, CL, OP); break;
	}
}

/** get_vdp_timing_value() **************************************/
/** Get timing value for a certain VDP command              **/
/*************************************************************/
int v99x8_device::get_vdp_timing_value(const int *timing_values)
{
	return(timing_values[((m_cont_reg[1]>>6)&1)|(m_cont_reg[8]&2)|((m_cont_reg[9]<<1)&4)]);
}

/** SrchEgine()** ********************************************/
/** Search a dot                                            **/
/*************************************************************/
void v99x8_device::srch_engine()
{
	int SX=m_mmc.SX;
	int SY=m_mmc.SY;
	int TX=m_mmc.TX;
	int ANX=m_mmc.ANX;
	UINT8 CL=m_mmc.CL;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(srch_timing);
	cnt = m_vdp_ops_count;

	#define post_srch(MX) \
	{ m_stat_reg[2]|=0x10; /* Border detected */ break; } \
	if ((SX+=TX) & MX) { m_stat_reg[2] &= 0xEF; /* Border not detected */ break; }
	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop if ((VDPpoint5(MXD, SX, SY)==CL) ^ANX)  post_srch(256) post_loop
			break;
	case V9938_MODE_GRAPHIC5: pre_loop if ((VDPpoint6(MXD, SX, SY)==CL) ^ANX)  post_srch(512) post_loop
			break;
	case V9938_MODE_GRAPHIC6: pre_loop if ((VDPpoint7(MXD, SX, SY)==CL) ^ANX)  post_srch(512) post_loop
			break;
	case V9938_MODE_GRAPHIC7: pre_loop if ((VDPpoint8(MXD, SX, SY)==CL) ^ANX)  post_srch(256) post_loop
			break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2] &= 0xFE;
		m_vdp_engine = NULL;
		// Update SX in VDP registers
		m_stat_reg[8] = SX & 0xFF;
		m_stat_reg[9] = (SX>>8) | 0xFE;
	}
	else {
		m_mmc.SX=SX;
	}
}

/** LineEgine()** ********************************************/
/** Draw a line                                             **/
/*************************************************************/
void v99x8_device::line_engine()
{
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ASX=m_mmc.ASX;
	int ADX=m_mmc.ADX;
	UINT8 CL=m_mmc.CL;
	UINT8 LO=m_mmc.LO;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(line_timing);
	cnt = m_vdp_ops_count;

	#define post_linexmaj(MX) \
	DX+=TX; \
	if ((ASX-=NY)<0) { \
		ASX+=NX; \
		DY+=TY; \
	} \
	ASX&=1023; /* Mask to 10 bits range */\
	if (ADX++==NX || (DX&MX)) \
		break; \
	post_loop

	#define post_lineymaj(MX) \
	DY+=TY; \
	if ((ASX-=NY)<0) { \
		ASX+=NX; \
		DX+=TX; \
	} \
	ASX&=1023; /* Mask to 10 bits range */\
	if (ADX++==NX || (DX&MX)) \
		break; \
	post_loop

	if ((m_cont_reg[45]&0x01)==0)
		// X-Axis is major direction
	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, DX, DY, CL, LO); post_linexmaj(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, DX, DY, CL, LO); post_linexmaj(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, DX, DY, CL, LO); post_linexmaj(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, DX, DY, CL, LO); post_linexmaj(256)
		break;
	}
	else
		// Y-Axis is major direction
	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, DX, DY, CL, LO); post_lineymaj(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, DX, DY, CL, LO); post_lineymaj(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, DX, DY, CL, LO); post_lineymaj(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, DX, DY, CL, LO); post_lineymaj(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.DX=DX;
		m_mmc.DY=DY;
		m_mmc.ASX=ASX;
		m_mmc.ADX=ADX;
	}
}

/** lmmv_engine() *********************************************/
/** VDP -> Vram                                             **/
/*************************************************************/
void v99x8_device::lmmv_engine()
{
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	UINT8 CL=m_mmc.CL;
	UINT8 LO=m_mmc.LO;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(lmmv_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, ADX, DY, CL, LO); post__x_y(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, ADX, DY, CL, LO); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, ADX, DY, CL, LO); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, ADX, DY, CL, LO); post__x_y(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		if (!NY)
			DY+=TY;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
	}
	else {
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ADX=ADX;
	}
}

/** lmmm_engine() *********************************************/
/** Vram -> Vram                                            **/
/*************************************************************/
void v99x8_device::lmmm_engine()
{
	int SX=m_mmc.SX;
	int SY=m_mmc.SY;
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ASX=m_mmc.ASX;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	UINT8 LO=m_mmc.LO;
	int MXS = m_mmc.MXS;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(lmmm_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, ADX, DY, VDPpoint5(MXS, ASX, SY), LO); post_xxyy(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, ADX, DY, VDPpoint6(MXS, ASX, SY), LO); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, ADX, DY, VDPpoint7(MXS, ASX, SY), LO); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, ADX, DY, VDPpoint8(MXS, ASX, SY), LO); post_xxyy(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		if (!NY) {
			SY+=TY;
			DY+=TY;
		}
		else
			if (SY==-1)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[34]=SY & 0xFF;
		m_cont_reg[35]=(SY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.SY=SY;
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ASX=ASX;
		m_mmc.ADX=ADX;
	}
}

/** lmcm_engine() *********************************************/
/** Vram -> CPU                                             **/
/*************************************************************/
void v99x8_device::lmcm_engine()
{
	if ((m_stat_reg[2]&0x80)!=0x80) {
		m_stat_reg[7]=m_cont_reg[44]=VDP_POINT(((m_mode >= 5) && (m_mode <= 8)) ? (m_mode-5) : 0, m_mmc.MXS, m_mmc.ASX, m_mmc.SY);
		m_vdp_ops_count-=get_vdp_timing_value(lmmv_timing);
		m_stat_reg[2]|=0x80;

		if (!--m_mmc.ANX || ((m_mmc.ASX+=m_mmc.TX)&m_mmc.MX)) {
			if (!(--m_mmc.NY & 1023) || (m_mmc.SY+=m_mmc.TY)==-1) {
				m_stat_reg[2]&=0xFE;
				m_vdp_engine=NULL;
				if (!m_mmc.NY)
					m_mmc.DY+=m_mmc.TY;
				m_cont_reg[42]=m_mmc.NY & 0xFF;
				m_cont_reg[43]=(m_mmc.NY>>8) & 0x03;
				m_cont_reg[34]=m_mmc.SY & 0xFF;
				m_cont_reg[35]=(m_mmc.SY>>8) & 0x03;
			}
			else {
				m_mmc.ASX=m_mmc.SX;
				m_mmc.ANX=m_mmc.NX;
			}
		}
	}
}

/** lmmc_engine() *********************************************/
/** CPU -> Vram                                             **/
/*************************************************************/
void v99x8_device::lmmc_engine()
{
	if ((m_stat_reg[2]&0x80)!=0x80) {
		UINT8 SM=((m_mode >= 5) && (m_mode <= 8)) ? (m_mode-5) : 0;

		m_stat_reg[7]=m_cont_reg[44]&=Mask[SM];
		VDP_PSET(SM, m_mmc.MXD, m_mmc.ADX, m_mmc.DY, m_cont_reg[44], m_mmc.LO);
		m_vdp_ops_count-=get_vdp_timing_value(lmmv_timing);
		m_stat_reg[2]|=0x80;

		if (!--m_mmc.ANX || ((m_mmc.ADX+=m_mmc.TX)&m_mmc.MX)) {
			if (!(--m_mmc.NY&1023) || (m_mmc.DY+=m_mmc.TY)==-1) {
				m_stat_reg[2]&=0xFE;
				m_vdp_engine=NULL;
				if (!m_mmc.NY)
					m_mmc.DY+=m_mmc.TY;
				m_cont_reg[42]=m_mmc.NY & 0xFF;
				m_cont_reg[43]=(m_mmc.NY>>8) & 0x03;
				m_cont_reg[38]=m_mmc.DY & 0xFF;
				m_cont_reg[39]=(m_mmc.DY>>8) & 0x03;
			}
			else {
				m_mmc.ADX=m_mmc.DX;
				m_mmc.ANX=m_mmc.NX;
			}
		}
	}
}

/** hmmv_engine() *********************************************/
/** VDP --> Vram                                            **/
/*************************************************************/
void v99x8_device::hmmv_engine()
{
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	UINT8 CL=m_mmc.CL;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(hmmv_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop m_vram_space->write_byte(VDP_VRMP5(MXD, ADX, DY), CL); post__x_y(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop m_vram_space->write_byte(VDP_VRMP6(MXD, ADX, DY), CL); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop m_vram_space->write_byte(VDP_VRMP7(MXD, ADX, DY), CL); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop m_vram_space->write_byte(VDP_VRMP8(MXD, ADX, DY), CL); post__x_y(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		if (!NY)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ADX=ADX;
	}
}

/** hmmm_engine() *********************************************/
/** Vram -> Vram                                            **/
/*************************************************************/
void v99x8_device::hmmm_engine()
{
	int SX=m_mmc.SX;
	int SY=m_mmc.SY;
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ASX=m_mmc.ASX;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	int MXS = m_mmc.MXS;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(hmmm_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop m_vram_space->write_byte(VDP_VRMP5(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP5(MXS, ASX, SY))); post_xxyy(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop m_vram_space->write_byte(VDP_VRMP6(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP6(MXS, ASX, SY))); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop m_vram_space->write_byte(VDP_VRMP7(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP7(MXS, ASX, SY))); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop m_vram_space->write_byte(VDP_VRMP8(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP8(MXS, ASX, SY))); post_xxyy(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		if (!NY) {
			SY+=TY;
			DY+=TY;
		}
		else
			if (SY==-1)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[34]=SY & 0xFF;
		m_cont_reg[35]=(SY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.SY=SY;
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ASX=ASX;
		m_mmc.ADX=ADX;
	}
}

/** ymmm_engine() *********************************************/
/** Vram -> Vram                                            **/
/*************************************************************/

void v99x8_device::ymmm_engine()
{
	int SY=m_mmc.SY;
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NY=m_mmc.NY;
	int ADX=m_mmc.ADX;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(ymmm_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop m_vram_space->write_byte(VDP_VRMP5(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP5(MXD, ADX, SY))); post__xyy(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop m_vram_space->write_byte(VDP_VRMP6(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP6(MXD, ADX, SY))); post__xyy(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop m_vram_space->write_byte(VDP_VRMP7(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP7(MXD, ADX, SY))); post__xyy(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop m_vram_space->write_byte(VDP_VRMP8(MXD, ADX, DY), m_vram_space->read_byte(VDP_VRMP8(MXD, ADX, SY))); post__xyy(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		if (!NY) {
			SY+=TY;
			DY+=TY;
		}
		else
			if (SY==-1)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[34]=SY & 0xFF;
		m_cont_reg[35]=(SY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.SY=SY;
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ADX=ADX;
	}
}

/** hmmc_engine() *********************************************/
/** CPU -> Vram                                             **/
/*************************************************************/
void v99x8_device::hmmc_engine()
{
	if ((m_stat_reg[2]&0x80)!=0x80) {
		m_vram_space->write_byte(VDP_VRMP(((m_mode >= 5) && (m_mode <= 8)) ? (m_mode-5) : 0, m_mmc.MXD, m_mmc.ADX, m_mmc.DY), m_cont_reg[44]);
		m_vdp_ops_count -= get_vdp_timing_value(hmmv_timing);
		m_stat_reg[2]|=0x80;

		if (!--m_mmc.ANX || ((m_mmc.ADX+=m_mmc.TX)&m_mmc.MX)) {
			if (!(--m_mmc.NY&1023) || (m_mmc.DY+=m_mmc.TY)==-1) {
				m_stat_reg[2]&=0xFE;
				m_vdp_engine=NULL;
				if (!m_mmc.NY)
					m_mmc.DY+=m_mmc.TY;
				m_cont_reg[42]=m_mmc.NY & 0xFF;
				m_cont_reg[43]=(m_mmc.NY>>8) & 0x03;
				m_cont_reg[38]=m_mmc.DY & 0xFF;
				m_cont_reg[39]=(m_mmc.DY>>8) & 0x03;
			}
			else {
				m_mmc.ADX=m_mmc.DX;
				m_mmc.ANX=m_mmc.NX;
			}
		}
	}
}

/** VDPWrite() ***********************************************/
/** Use this function to transfer pixel(s) from CPU to m_ **/
/*************************************************************/
void v99x8_device::cpu_to_vdp(UINT8 V)
{
	m_stat_reg[2]&=0x7F;
	m_stat_reg[7]=m_cont_reg[44]=V;
	if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();
}

/** VDPRead() ************************************************/
/** Use this function to transfer pixel(s) from VDP to CPU. **/
/*************************************************************/
UINT8 v99x8_device::vdp_to_cpu()
{
	m_stat_reg[2]&=0x7F;
	if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();
	return(m_cont_reg[44]);
}

/** report_vdp_command() ***************************************/
/** Report VDP Command to be executed                       **/
/*************************************************************/
void v99x8_device::report_vdp_command(UINT8 Op)
{
	static const char *const Ops[16] =
	{
		"SET ","AND ","OR  ","XOR ","NOT ","NOP ","NOP ","NOP ",
		"TSET","TAND","TOR ","TXOR","TNOT","NOP ","NOP ","NOP "
	};
	static const char *const Commands[16] =
	{
		" ABRT"," ????"," ????"," ????","POINT"," PSET"," SRCH"," LINE",
		" LMMV"," LMMM"," LMCM"," LMMC"," HMMV"," HMMM"," YMMM"," HMMC"
	};

	UINT8 CL, CM, LO;
	int SX,SY, DX,DY, NX,NY;

	// Fetch arguments
	CL = m_cont_reg[44];
	SX = (m_cont_reg[32]+((int)m_cont_reg[33]<<8)) & 511;
	SY = (m_cont_reg[34]+((int)m_cont_reg[35]<<8)) & 1023;
	DX = (m_cont_reg[36]+((int)m_cont_reg[37]<<8)) & 511;
	DY = (m_cont_reg[38]+((int)m_cont_reg[39]<<8)) & 1023;
	NX = (m_cont_reg[40]+((int)m_cont_reg[41]<<8)) & 1023;
	NY = (m_cont_reg[42]+((int)m_cont_reg[43]<<8)) & 1023;
	CM = Op>>4;
	LO = Op&0x0F;

	LOG(("V9938: Opcode %02Xh %s-%s (%d,%d)->(%d,%d),%d [%d,%d]%s\n",
		Op, Commands[CM], Ops[LO],
		SX,SY, DX,DY, CL, m_cont_reg[45]&0x04? -NX:NX,
		m_cont_reg[45]&0x08? -NY:NY,
		m_cont_reg[45]&0x70? " on ExtVRAM":""
		));
}

/** VDPDraw() ************************************************/
/** Perform a given V9938 operation Op.                     **/
/*************************************************************/
UINT8 v99x8_device::command_unit_w(UINT8 Op)
{
	int SM;

	// V9938 ops only work in SCREENs 5-8
	if (m_mode<5)
		return(0);

	SM = m_mode-5;         // Screen mode index 0..3

	m_mmc.CM = Op>>4;
	if ((m_mmc.CM & 0x0C) != 0x0C && m_mmc.CM != 0)
		// Dot operation: use only relevant bits of color
	m_stat_reg[7]=(m_cont_reg[44]&=Mask[SM]);

	//  if(Verbose&0x02)
	report_vdp_command(Op);

	switch(Op>>4) {
	case CM_ABRT:
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		return 1;
	case CM_POINT:
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		m_stat_reg[7]=m_cont_reg[44]=
		VDP_POINT(SM, (m_cont_reg[45] & 0x10) != 0,
			m_cont_reg[32]+((int)m_cont_reg[33]<<8),
			m_cont_reg[34]+((int)m_cont_reg[35]<<8));
		return 1;
	case CM_PSET:
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=NULL;
		VDP_PSET(SM, (m_cont_reg[45] & 0x20) != 0,
			m_cont_reg[36]+((int)m_cont_reg[37]<<8),
			m_cont_reg[38]+((int)m_cont_reg[39]<<8),
			m_cont_reg[44],
			Op&0x0F);
		return 1;
	case CM_SRCH:
		m_vdp_engine=&v99x8_device::srch_engine;
		break;
	case CM_LINE:
		m_vdp_engine=&v99x8_device::line_engine;
		break;
	case CM_LMMV:
		m_vdp_engine=&v99x8_device::lmmv_engine;
		break;
	case CM_LMMM:
		m_vdp_engine=&v99x8_device::lmmm_engine;
		break;
	case CM_LMCM:
		m_vdp_engine=&v99x8_device::lmcm_engine;
		break;
	case CM_LMMC:
		m_vdp_engine=&v99x8_device::lmmc_engine;
		break;
	case CM_HMMV:
		m_vdp_engine=&v99x8_device::hmmv_engine;
		break;
	case CM_HMMM:
		m_vdp_engine=&v99x8_device::hmmm_engine;
		break;
	case CM_YMMM:
		m_vdp_engine=&v99x8_device::ymmm_engine;
		break;
	case CM_HMMC:
		m_vdp_engine=&v99x8_device::hmmc_engine;
		break;
	default:
		LOG(("V9938: Unrecognized opcode %02Xh\n",Op));
		return(0);
	}

	// Fetch unconditional arguments
	m_mmc.SX = (m_cont_reg[32]+((int)m_cont_reg[33]<<8)) & 511;
	m_mmc.SY = (m_cont_reg[34]+((int)m_cont_reg[35]<<8)) & 1023;
	m_mmc.DX = (m_cont_reg[36]+((int)m_cont_reg[37]<<8)) & 511;
	m_mmc.DY = (m_cont_reg[38]+((int)m_cont_reg[39]<<8)) & 1023;
	m_mmc.NY = (m_cont_reg[42]+((int)m_cont_reg[43]<<8)) & 1023;
	m_mmc.TY = m_cont_reg[45]&0x08? -1:1;
	m_mmc.MX = PPL[SM];
	m_mmc.CL = m_cont_reg[44];
	m_mmc.LO = Op&0x0F;
	m_mmc.MXS = (m_cont_reg[45] & 0x10) != 0;
	m_mmc.MXD = (m_cont_reg[45] & 0x20) != 0;

	// Argument depends on UINT8 or dot operation
	if ((m_mmc.CM & 0x0C) == 0x0C) {
		m_mmc.TX = m_cont_reg[45]&0x04? -PPB[SM]:PPB[SM];
		m_mmc.NX = ((m_cont_reg[40]+((int)m_cont_reg[41]<<8)) & 1023)/PPB[SM];
	}
	else {
		m_mmc.TX = m_cont_reg[45]&0x04? -1:1;
		m_mmc.NX = (m_cont_reg[40]+((int)m_cont_reg[41]<<8)) & 1023;
	}

	// X loop variables are treated specially for LINE command
	if (m_mmc.CM == CM_LINE) {
		m_mmc.ASX=((m_mmc.NX-1)>>1);
		m_mmc.ADX=0;
	}
	else {
		m_mmc.ASX = m_mmc.SX;
		m_mmc.ADX = m_mmc.DX;
	}

	// NX loop variable is treated specially for SRCH command
	if (m_mmc.CM == CM_SRCH)
		m_mmc.ANX=(m_cont_reg[45]&0x02)!=0; // Do we look for "==" or "!="?
	else
		m_mmc.ANX = m_mmc.NX;

	// Command execution started
	m_stat_reg[2]|=0x01;

	// Start execution if we still have time slices
	if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();

	// Operation successfully initiated
	return(1);
}

/** LoopVDP() ************************************************
Run X steps of active VDP command
*************************************************************/
void v99x8_device::update_command()
{
	if(m_vdp_ops_count<=0)
	{
		m_vdp_ops_count+=13662;
		if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();
	}
	else
	{
		m_vdp_ops_count=13662;
		if(m_vdp_engine) (this->*m_vdp_engine)();
	}
}

/*static MACHINE_CONFIG_FRAGMENT( v9938 )
	MCFG_PALETTE_ADD("palette", 512)
	MCFG_PALETTE_INIT_OWNER(v9938_device, v9938)
MACHINE_CONFIG_END*/

//-------------------------------------------------
//  machine_config_additions - return a pointer to
//  the device's machine fragment
//-------------------------------------------------

/*machine_config_constructor v9938_device::device_mconfig_additions() const
{
	return MACHINE_CONFIG_NAME( v9938 );
}*/

/*static MACHINE_CONFIG_FRAGMENT( v9958 )
	MCFG_PALETTE_ADD("palette", 19780)
	MCFG_PALETTE_INIT_OWNER(v9958_device, v9958)
MACHINE_CONFIG_END*/

//-------------------------------------------------
//  machine_config_additions - return a pointer to
//  the device's machine fragment
//-------------------------------------------------

/*machine_config_constructor v9958_device::device_mconfig_additions() const
{
	return MACHINE_CONFIG_NAME( v9958 );
}*/



/* for common source code project */
void v99x8_device::prepare_screen()
{
	if (emu->now_waiting_in_debugger) {
		// store regs
//		int tmp_offset_x = m_offset_x;
		int tmp_offset_y = m_offset_y;
		int tmp_visible_y = m_visible_y;
		UINT8 tmp_stat_reg[10];
		memcpy(tmp_stat_reg, m_stat_reg, sizeof(m_stat_reg));
		UINT8 tmp_int_state = m_int_state;
		int tmp_scanline = m_scanline;
		int tmp_blink = m_blink;
		int tmp_blink_count = m_blink_count;
		UINT16 tmp_pal_ind16[16];
		UINT16 tmp_pal_ind256[256];
		memcpy(tmp_pal_ind16, m_pal_ind16, sizeof(m_pal_ind16));
		memcpy(tmp_pal_ind256, m_pal_ind256, sizeof(m_pal_ind256));
		//		mmc_t tmp_mmc = m_mmc;
		int tmp_vdp_ops_count = m_vdp_ops_count;
		int tmp_scanline_start = m_scanline_start;
		int tmp_scanline_max = m_scanline_max;

		// drive vlines
		for (int v = /*get_cur_vline() + 1*/0; v < get_lines_per_frame(); v++) {
			event_vline(v, 0);
		}

		// restore regs
//		m_offset_x = tmp_offset_x;
		m_offset_y = tmp_offset_y;
		m_visible_y = tmp_visible_y;
		memcpy(m_stat_reg, tmp_stat_reg, sizeof(m_stat_reg));
		m_int_state = tmp_int_state;
		m_scanline = tmp_scanline;
		m_blink = tmp_blink;
		m_blink_count = tmp_blink_count;
		memcpy(m_pal_ind16, tmp_pal_ind16, sizeof(m_pal_ind16));
		memcpy(m_pal_ind256, tmp_pal_ind256, sizeof(m_pal_ind256));
		//		m_mmc = tmp_mmc;
		m_vdp_ops_count = tmp_vdp_ops_count;
		m_scanline_start = tmp_scanline_start;
		m_scanline_max = tmp_scanline_max;
	}
}

void v99x8_device::draw_screen()
{
	prepare_screen();
	scrntype_t* dst;
	int y;
	for (y = 0; y < SCREEN_HEIGHT; y++) {
		if ((dst = emu->get_screen_buffer(y)) != NULL) {
			my_memcpy(dst, screen + (y + 18) * LONG_WIDTH + 2, SCREEN_WIDTH * sizeof(scrntype_t));
		}
	}
}

void v99x8_device::draw_line_transparent(scrntype_t* dst, int y)
{
	if (dst != NULL) {
		scrntype_t* src = screen + (y + 18) * LONG_WIDTH + 2;
		scrntype_t* tp = transparent + (y + 18) * LONG_WIDTH + 2;
		for (int x = 0; x < SCREEN_WIDTH; x++) {
			if (tp[x] != 0) {
				dst[x] = src[x];
			}
		}
	}
}


void v99x8_device::initialize()
{
	device_start();
	register_vline_event(this);
}

void v99x8_device::reset()
{
	device_reset();
}

void v99x8_device::event_vline(int v, int clock)
{
	m_stat_reg[2] ^= 0x20;
	device_timer(v);
}

void v99x8_device::write_signal(int id, uint32_t data, uint32_t mask)
{
	if(id == SIG_VDP_COMMAND_COMPLETION) {
		while(1) {
			int i;
			i = m_vdp_ops_count = 13662;
			if(m_vdp_engine) (this->*m_vdp_engine)();
			if (i == m_vdp_ops_count) break;
		}
	}
}

#define STATE_VERSION	2

bool v99x8_device::process_state(FILEIO* state_fio, bool loading)
{
	if(!state_fio->StateCheckUint32(STATE_VERSION)) {
		return false;
	}
	if(!state_fio->StateCheckInt32(this_device_id)) {
		return false;
	}
	save_load_state(state_fio, !loading);
	return true;
}

void v99x8_device::save_load_state(FILEIO* state_fio, bool is_save)
{
#define STATE_ENTRY(x) {&(x), sizeof(x)}
	typedef struct {
		void *address;
		size_t size;
	} t_state_table;
	t_state_table state_table[] = {
		STATE_ENTRY(m_offset_x),
		STATE_ENTRY(m_offset_y),
		STATE_ENTRY(m_visible_y),
		STATE_ENTRY(m_mode),
		STATE_ENTRY(m_pal_write_first),
		STATE_ENTRY(m_cmd_write_first),
		STATE_ENTRY(m_pal_write),
		STATE_ENTRY(m_cmd_write),
		STATE_ENTRY(m_pal_reg),
		STATE_ENTRY(m_stat_reg),
		STATE_ENTRY(m_cont_reg),
		STATE_ENTRY(m_read_ahead),
		STATE_ENTRY(m_int_state),
		STATE_ENTRY(m_scanline),
		STATE_ENTRY(m_blink),
		STATE_ENTRY(m_blink_count),
		STATE_ENTRY(m_mx_delta),
		STATE_ENTRY(m_my_delta),
		STATE_ENTRY(m_button_state),
		STATE_ENTRY(m_pal_ind16),
		STATE_ENTRY(m_pal_ind256),
		STATE_ENTRY(m_mmc),
		STATE_ENTRY(m_vdp_ops_count),
		STATE_ENTRY(m_pal_ntsc),
		STATE_ENTRY(m_scanline_start),
		STATE_ENTRY(m_vblank_start),
		STATE_ENTRY(m_scanline_max),
		STATE_ENTRY(m_height),
		STATE_ENTRY(m_v9958_sp_mode),
		STATE_ENTRY(m_address_latch),
		STATE_ENTRY(vram),
		{ NULL, 0 }
	};
	int i;
	for(i=0; state_table[i].size>0; i++) {
		if (is_save) {
			state_fio->Fwrite(state_table[i].address, state_table[i].size, 1);
		}
		else {
			state_fio->Fread(state_table[i].address, state_table[i].size, 1);
		}
	}
	return;
}

/*
	Common Source Code Project
	MSX Series (experimental)

	Origin : mame0172s.zip
		mame.zip\src\devices\video\v9938.cpp
	modified by umaiboux
	Date   : 2016.04.xx-

	[ V99x8 ]
*/
