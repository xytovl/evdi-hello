#include <atomic>
#include <bits/types/struct_timeval.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <stdint.h>
#include <evdi_lib.h>
#include <iostream>

#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <vector>
#include <thread>
#include <condition_variable>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include "edid.h"

static constexpr unsigned int alignment = 128;

class EvdiDevice
{
public:
	EvdiDevice(const EDID & edid, int maxwidth, int maxheight)
	{
		handle = evdi_open(1);
		if (handle == EVDI_INVALID_HANDLE)
			throw std::runtime_error("failed to open evdi device");
		evdi_connect(handle, edid.data(), edid.size(), maxwidth * maxheight);
	}

	~EvdiDevice()
	{
		for (std::size_t i = 0 ; i  < buffers.size() ; ++i)
		{
			if (not buffers[i].empty())
			{
				evdi_unregister_buffer(handle, i);
			}
		}

		evdi_disconnect(handle);
		evdi_close(handle);
	}

	const std::vector<char> & get_buffer()
	{
		int next_buffer = (last_buffer + 1) % buffers.size();
		int evdi_fd = evdi_get_event_ready(handle);

		evdi_event_context handlers{
			.dpms_handler = dpms_handler,
				.mode_changed_handler = mode_changed_handler,
				.update_ready_handler = update_ready_handler,
				.crtc_state_handler = nullptr,
				.cursor_set_handler = nullptr,
				.cursor_move_handler = nullptr,
				.user_data = this 
		};
		while (buffers[0].empty())
		{
			fd_set read_fd, write_fd, except_fd;
			FD_ZERO(&read_fd);
			FD_SET(evdi_fd, &read_fd);
			FD_ZERO(&write_fd);
			FD_ZERO(&except_fd);
			int count = select(evdi_fd + 1, &read_fd, &write_fd, &except_fd, NULL);
			if (count < 0) {
				std::cerr << strerror(errno) << ": " << count << std::endl;
				std::abort();
			} else if (count == 1)
			{
				std::cout << "events ready to process" << std::endl;
				evdi_handle_events(handle, &handlers);
			}
		}
		if (evdi_request_update(handle, next_buffer))
			update_ready_handler(next_buffer, this);
		else
		{
			while (next_buffer != last_buffer)
			{
				fd_set read_fd, write_fd, except_fd;
				FD_ZERO(&read_fd);
				FD_SET(evdi_fd, &read_fd);
				FD_ZERO(&write_fd);
				FD_ZERO(&except_fd);
				int count = select(evdi_fd + 1, &read_fd, &write_fd, &except_fd, NULL);
				if (count < 0) {
					std::cerr << strerror(errno) << ": " << count << std::endl;
					std::abort();
				} else if (count == 1)
				{
					std::cout << "events ready to process" << std::endl;
					evdi_handle_events(handle, &handlers);
				}
			}
		}
		return buffers[last_buffer];
	}

private:
	evdi_handle handle;
	int last_buffer = 0;

	std::array<std::vector<char>, 2> buffers;
	evdi_mode current_mode;

	static void dpms_handler(int dpms_mode, void *user_data);
	static void mode_changed_handler(struct evdi_mode mode, void *user_data);
	static void update_ready_handler(int buffer_to_be_updated, void *user_data);
	static void crtc_state_handler(int state, void *user_data);
	static void cursor_set_handler(struct evdi_cursor_set cursor_set, void *user_data);
	static void cursor_move_handler(struct evdi_cursor_move cursor_move, void *user_data);
};

void EvdiDevice::dpms_handler(int dpms_mode, void* user_data)
{
	(void) dpms_mode;
	(void) user_data;
}

void EvdiDevice::mode_changed_handler(evdi_mode mode, void *user_data)
{
	EvdiDevice &self = *(EvdiDevice *)user_data;
	std::cerr << "mode changed: " << mode.width << "x" << mode.height
		<< "(" << mode.bits_per_pixel << "bpp "
		<< (char)mode.pixel_format
		<< (char)(mode.pixel_format >> 8)
		<< (char)(mode.pixel_format >> 16)
		<< (char)(mode.pixel_format >> 24)
		<< ")" << std::endl;

	if (self.current_mode.width * self.current_mode.height * self.current_mode.bits_per_pixel
			!= mode.width * mode.height * mode.bits_per_pixel)
	{
		for (std::size_t i = 0 ; i < self.buffers.size() ; ++i)
		{
			auto & buffer = self.buffers[i];
			if (not buffer.empty())
			{
				evdi_unregister_buffer(self.handle, i);
			}
			buffer.resize(alignment + mode.width * mode.height * mode.bits_per_pixel / 8);
			buffer.shrink_to_fit();

			evdi_buffer ev_buffer{
				.id = int(i),
					.buffer = buffer.data() + (intptr_t(buffer.data()) % alignment),
					.width = mode.width,
					.height = mode.height,
					.stride = mode.width * mode.bits_per_pixel / 8,
					.rects = nullptr,
					.rect_count = 0
			};

			evdi_register_buffer(self.handle, ev_buffer);
		}
	}
	self.current_mode = mode;
}

void EvdiDevice::update_ready_handler(int buffer_to_be_updated, void *user_data)
{
	EvdiDevice &self = *(EvdiDevice *)user_data;

	std::array<evdi_rect, 16> rects;
	int num_rects;
	auto start = std::chrono::steady_clock::now();
	evdi_grab_pixels(self.handle, rects.data(), &num_rects);
	auto now = std::chrono::steady_clock::now();
	std::cout << "grab_pixel time(us): " << std::chrono::duration_cast<std::chrono::microseconds>(now - start).count()
		<< " rects: " << num_rects << std::endl;
	for (int i = 0 ; i < num_rects ; ++i)
	{
		std::cout << "  " << rects[i].x1 << "-" << rects[i].x2 << "x" << rects[i].y1 << "-" << rects[i].y2 << std::endl;
	}

	self.last_buffer = buffer_to_be_updated;
}

int main()
{
	EDID edid("PVR");
	edid.add_mode(1920, EDID::aspect_ratio::r16_9, 60);

	EvdiDevice device(edid, 1920, 1080);

	int frame = 1000;
	for (
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	;//std::chrono::steady_clock::now() < end;
	++frame)
	{
		std::string filename = "frame" + std::to_string(frame).substr(1) + ".png";
		const auto & buffer = device.get_buffer();
		if (not buffer.empty())
			stbi_write_png(filename.c_str(), 1920, 1080, 4, buffer.data()+1 + (intptr_t(buffer.data()) % alignment), 1920*4);
	}

}
