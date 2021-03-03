#include "edid.h"

#include <numeric>

static unsigned char letter(const char c) 
{
	return c + 1 - 'A';
}

EDID::EDID(const char vendor[3])
{
	buffer.fill(0);
	// fixed header 00 FF FF FF FF FF FF 00
	for (std::size_t i = 1; i < 7; ++i)
		buffer[i]  = 0xFF;

	uint16_t vendor_edid =
		letter(vendor[0]) << 10
		| letter(vendor[1]) << 5
		| letter(vendor[2]);
	buffer[9] = vendor_edid;
	buffer[8] = vendor_edid >> 8;
	buffer[10] = 1;
	buffer[11] = 0;

	buffer[18] = 1; // EDID version
	buffer[19] = 4; // EDID revision

	buffer[20] = 1 << 7 // digital input
		| 2 << 4 // 8bits per color
		| 5; // displayport

	std::fill(buffer.begin() + 38, buffer.begin() + 54, 1);

	checksum();
}

void EDID::checksum()
{
	buffer[127] = -std::accumulate(buffer.begin(), buffer.end() - 1, (unsigned char)0);
}

void EDID::add_mode(int xres, aspect_ratio ratio, int freq)
	{
		buffer[38 + 2*modes] = xres / 8 - 31;
		buffer[39 + 2*modes] = char(ratio) << 6 | (freq - 60);
		++modes;
		checksum();
	}

