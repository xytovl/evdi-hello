#include <array>
#include <cstdint>
#include <fstream>

class EDID
{
public:
	EDID(const char vendor[3]);

	enum class aspect_ratio: char
	{
		r16_10 = 0,
		r4_3 = 1,
		r5_4 = 2,
		r16_9 = 3
	};

	void add_mode(int xres, aspect_ratio ratio, int freq);

	const unsigned char* data() const
	{
		return buffer.data();
	}
	std::size_t size() const
	{
		return buffer.size();
	}
private:
	void checksum();
	std::array<unsigned char, 128> buffer;
	int modes = 0;
};
