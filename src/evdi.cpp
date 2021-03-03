#include <atomic>
#include <bits/types/struct_timeval.h>
#include <chrono>
#include <cstddef>
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

class EvdiDevice
{
public:
	EvdiDevice(const EDID & edid, int maxwidth, int maxheight, double fps)
	{
		handle = evdi_open(1);
		frame_duration = std::chrono::duration_cast<decltype(frame_duration)>(std::chrono::duration<double>(1/fps));
		if (handle == EVDI_INVALID_HANDLE)
			throw std::runtime_error("failed to open evdi device");
		//mode_changed_handler(evdi_mode{maxwidth, maxheight, int(fps), 4, 0}, this);
		evdi_connect(handle, edid.data(), edid.size(), maxwidth * maxheight);

		thread = std::thread([this](){run();});
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

	void stop() {
		stop_request = true;
		thread.join();
	}

	const std::vector<char> & get_buffer()
	{
		std::unique_lock<std::mutex> lock(index_mutex);
		while (user_buffer == last_buffer)
		{
			auto start = std::chrono::steady_clock::now();
			buffer_ready.wait(lock);
			std::cout << "waited for frame " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() <<std::endl;
		}
		user_buffer = last_buffer;
		buffer_ready.notify_all();
		return buffers[user_buffer];
	}

private:
	void run()
	{
		int evdi_fd = evdi_get_event_ready(handle);
		auto next_frame = std::chrono::steady_clock::now();

		evdi_event_context handlers{
			.dpms_handler = dpms_handler,
				.mode_changed_handler = mode_changed_handler,
				.update_ready_handler = update_ready_handler,
				.crtc_state_handler = nullptr,
				.cursor_set_handler = nullptr,
				.cursor_move_handler = nullptr,
				.user_data = this 
		};
		while (not stop_request)
		{
			timeval timeout{
				.tv_sec = 0,
					.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(next_frame - std::chrono::steady_clock::now()).count()
			};
			fd_set read_fd, write_fd, except_fd;
			FD_ZERO(&read_fd);
			FD_SET(evdi_fd, &read_fd);
			FD_ZERO(&write_fd);
			FD_ZERO(&except_fd);
			int count = select(evdi_fd + 1, &read_fd, &write_fd, &except_fd, &timeout);
			if (count < 0) {
				std::cerr << strerror(errno) << ": " << count << std::endl;
				std::abort();
			} else if (count == 1)
			{
				std::cout << "events ready to process" << std::endl;
				evdi_handle_events(handle, &handlers);
			}
			std::this_thread::sleep_until(next_frame);
			int next_buffer;
			{
				std::unique_lock<std::mutex> lock(index_mutex);
				next_buffer = (last_buffer + 1) % buffers.size();
				if (next_buffer == user_buffer)
				{
					next_buffer = (next_buffer + 1) % buffers.size();
				}
			}
			if (not buffers[0].empty() && evdi_request_update(handle, next_buffer))
				update_ready_handler(next_buffer, this);
			while (next_frame < std::chrono::steady_clock::now())
			{
				next_frame += frame_duration;
			}
		}
	}
	evdi_handle handle;

	std::mutex index_mutex;
	int last_buffer = 0;
	int user_buffer = 0;
	std::condition_variable buffer_ready;
	std::array<std::vector<char>, 3> buffers;
	evdi_mode current_mode;

	std::thread thread;
	std::atomic_bool stop_request{false};
	std::chrono::microseconds frame_duration;

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
	std::cerr << "mode changed: " << mode.width << "x" << mode.height << std::endl;

	if (self.current_mode.width * self.current_mode.height * self.current_mode.bits_per_pixel
			!= mode.width * mode.height * mode.bits_per_pixel)
	{
		for (std::size_t i = 0 ; i < self.buffers.size() ; ++i)
		{
			{
				std::unique_lock<std::mutex> lock(self.index_mutex);
				while (not self.buffers[i].empty() and int(i) == self.user_buffer)
				{
					self.buffer_ready.wait(lock);
				}
			}
			auto & buffer = self.buffers[i];
			if (not buffer.empty())
			{
				evdi_unregister_buffer(self.handle, i);
			}
			buffer.resize(mode.width * mode.height * mode.bits_per_pixel);
			buffer.shrink_to_fit();

			evdi_buffer ev_buffer{
				.id = int(i),
					.buffer = buffer.data(),
					.width = mode.width,
					.height = mode.height,
					.stride = mode.width * mode.bits_per_pixel,
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
	std::cout << "grab_pixel time(ms): " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << std::endl;

	std::unique_lock<std::mutex> lock(self.index_mutex);
	if (self.last_buffer != self.user_buffer) {
		std::cout << "frame lost" << std::endl;
	}
	self.last_buffer = buffer_to_be_updated;
	self.buffer_ready.notify_all();
}

int main()
{
	EDID edid("PVR");
	edid.add_mode(1920, EDID::aspect_ratio::r16_9, 60);

	EvdiDevice device(edid, 1920, 1080, 60);

	int frame = 1000;
	for (
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	;//std::chrono::steady_clock::now() < end;
	++frame)
	{
		std::string filename = "frame" + std::to_string(frame).substr(1) + ".png";
		const auto & buffer = device.get_buffer();
    stbi_write_png(filename.c_str(), 1920, 1080, 4, buffer.data(), 1920*4);
	}

	device.stop();

}
