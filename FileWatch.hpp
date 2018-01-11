#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include <Pathcch.h>
#include <shlwapi.h>
#endif // WIN32

#if __unix__
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#endif // __unix__

#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>
#include <array>
#include <map>
#include <system_error>
#include <string>
#include <algorithm>
#include <type_traits>

namespace filewatch {
	enum class Event {
		added,
		removed,
		modified,
		renamed_old,
		renamed_new
	};

	template<
		class T,
		typename std::enable_if<std::is_integral<T>::value>::value
	>
	using string_type = std::basic_string<T, std::char_traits<T>>;

	template<
		class T,
		typename std::enable_if <
			std::is_same<
				T, std::basic_string<char, std::char_traits<char>>
			>::value ||
			std::is_same<
				T, std::basic_string<wchar_t, std::char_traits<wchar_t>>
			>::value
		>
	>
	using string_type = T;

	template<
		class T
	>
	class FileWatch
	{
	public:
		FileWatch(T path, std::function<void(const T& file, const Event event_type)> callback) :
			_path(path),
			_callback(callback),
			_directory(get_directory(path))
		{
#ifdef _WIN32
			_close_event = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (!_close_event) {
				throw std::system_error(GetLastError(), std::system_category());
			}
#endif // WIN32
			_callback_thread = std::move(std::thread([this]() { callback_thread(); }));
			_watch_thread = std::move(std::thread([this]() { monitor_directory(); }));
		}
		~FileWatch() {
			_destory = true;
#ifdef _WIN32
			SetEvent(_close_event);
#endif // WIN32
#if __unix__
			inotify_rm_watch(_directory.folder, _directory.watch);
#endif // __unix__
			cv.notify_all();
			_watch_thread.join();
			_callback_thread.join();
#ifdef _WIN32
			CloseHandle(_directory);
#endif // WIN32
#if __unix__
			close(_directory.folder);
#endif // __unix__
		}

	private:
		struct PathParts
		{
			PathParts(T directory, T filename) : directory(directory), filename(filename) {}
			T directory;
			T filename;
		};
		T _path;

		static constexpr std::size_t _buffer_size = { 1024 * 256 };

		// only used if watch a single file
		bool _watching_single_file = { false };
		string_type _filename;

		std::atomic<bool> _destory = { false };
		std::function<void(const T& file, const Event event_type)> _callback;

		std::thread _watch_thread;

		std::condition_variable cv;
		std::mutex _callback_mutex;
		std::vector<std::pair<T, Event>> _callback_information;
		std::thread _callback_thread;
#ifdef _WIN32
		HANDLE _directory = { nullptr };
		HANDLE _close_event = { nullptr };

		const DWORD _listen_filters =
			FILE_NOTIFY_CHANGE_SECURITY |
			FILE_NOTIFY_CHANGE_CREATION |
			FILE_NOTIFY_CHANGE_LAST_ACCESS |
			FILE_NOTIFY_CHANGE_LAST_WRITE |
			FILE_NOTIFY_CHANGE_SIZE |
			FILE_NOTIFY_CHANGE_ATTRIBUTES |
			FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_FILE_NAME;

		const std::map<DWORD, Event> _event_type_mapping = {
			{ FILE_ACTION_ADDED, Event::added },
			{ FILE_ACTION_REMOVED, Event::removed },
			{ FILE_ACTION_MODIFIED, Event::modified },
			{ FILE_ACTION_RENAMED_OLD_NAME, Event::renamed_old },
			{ FILE_ACTION_RENAMED_NEW_NAME, Event::renamed_new }
		};
#endif // WIN32

#if __unix__
		struct FolderInfo {
			int folder;
			int watch;
		};

		FolderInfo  _directory;

		const std::uint32_t _listen_filters = IN_MODIFY | IN_CREATE | IN_DELETE;

		const static std::size_t event_size = (sizeof(struct inotify_event));
#endif // __unix__

		const PathParts split_directory_and_file(const T& path) const {
			const auto predict = [](typename T::value_type character) {
#ifdef _WIN32
				return character == _T('\\') || character == _T('/');
#elif __unix__
				return character == '/';
#endif // __unix__
			};
#ifdef _WIN32
#define _UNICODE
			const string_type this_directory = _T("./");
#elif __unix__
			const string_type this_directory = "./";
#endif // __unix__
			
			const auto pivot = std::find_if(path.rbegin(), path.rend(), predict).base();
			//if the path is something like "test.txt" there will be no directoy part, however we still need one, so insert './'
			const T directory = [&]() {
				const auto extracted_directory = string_type(path.begin(), pivot);
				return (extracted_directory.size() > 0) ? extracted_directory : this_directory;
			}(); 
			const T filename = string_type(pivot, path.end());
			return PathParts(directory, filename);
		}

		bool pass_filter(const string_type file_path)
		{ 
			if (_watching_single_file) {
				const string_type extracted_filename = { split_directory_and_file(file_path).filename };
				//if we are watching a single file, only that file should trigger action
				return extracted_filename == _filename;
			}
			return true;
		}

#ifdef _WIN32
		HANDLE get_directory(const T& path) {
			auto file_info = GetFileAttributes(path.c_str());

			if (file_info == INVALID_FILE_ATTRIBUTES)
			{
				throw std::system_error(GetLastError(), std::system_category());
			}
			_watching_single_file = (file_info & FILE_ATTRIBUTE_DIRECTORY) == false;

			const T watch_path = [this, &path]() {
				if (_watching_single_file)
				{
					const auto parsed_path = split_directory_and_file(path);
					_filename = parsed_path.filename;
					return parsed_path.directory;
				}
				else 
				{
					return path;
				}
			}();

			HANDLE directory = ::CreateFile(
				watch_path.c_str(),           // pointer to the file name
				FILE_LIST_DIRECTORY,    // access (read/write) mode
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // share mode
				NULL, // security descriptor
				OPEN_EXISTING,         // how to create
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, // file attributes
				NULL);                 // file with attributes to copy

			if (directory == INVALID_HANDLE_VALUE)
			{
				throw std::system_error(GetLastError(), std::system_category());
			}
			return directory;
		}
		void monitor_directory() {
			std::vector<BYTE> buffer(_buffer_size);
			DWORD bytes_returned = 0;
			OVERLAPPED overlapped_buffer{ 0 };

			overlapped_buffer.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (!overlapped_buffer.hEvent) {
				std::cerr << "Error creating monitor event" << std::endl;
			}

			std::array<HANDLE, 2> handles{ overlapped_buffer.hEvent, _close_event };

			auto async_pending = false;
			do {
				std::vector<std::pair<T, Event>> parsed_information;
				ReadDirectoryChangesW(
					_directory,
					buffer.data(), buffer.size(),
					TRUE,
					_listen_filters,
					&bytes_returned,
					&overlapped_buffer, NULL);
				async_pending = true;
				switch (WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE))
				{
				case WAIT_OBJECT_0:
				{
					if (!GetOverlappedResult(_directory, &overlapped_buffer, &bytes_returned, TRUE)) {

					}
					async_pending = false;

					if (bytes_returned == 0) {
						break;
					}

					FILE_NOTIFY_INFORMATION *file_information = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(&buffer[0]);
					do
					{
						string_type changed_file{ file_information->FileName, file_information->FileNameLength / 2 };
						if (pass_filter(changed_file))
						{
							parsed_information.emplace_back(T{ changed_file }, _event_type_mapping.at(file_information->Action));
						}

						if (file_information->NextEntryOffset == 0) {
							break;
						}

						file_information = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(file_information) + file_information->NextEntryOffset);
					} while (true);
					break;
				}
				case WAIT_OBJECT_0 + 1:
					// quit
					break;
				case WAIT_FAILED:
					break;
				}
				//dispatch callbacks
				{
					std::lock_guard<std::mutex> lock(_callback_mutex);
					_callback_information.insert(_callback_information.end(), parsed_information.begin(), parsed_information.end());
				}
				cv.notify_all();
			} while (_destory == false);

			if (async_pending)
			{
				//clean up running async io
				CancelIo(_directory);
				GetOverlappedResult(_directory, &overlapped_buffer, &bytes_returned, TRUE);
			}
		}
#endif // WIN32

#if __unix__

		bool is_file(const T& path) const
		{
			struct stat statbuf;
			if (stat(path.c_str(), &statbuf) != 0)
			{
				throw std::system_error(errno, std::system_category());
			}
			return S_ISREG(statbuf.st_mode);
		}

		FolderInfo get_directory(const T& path) 
		{
			const auto folder = inotify_init();
			if (folder < 0) 
			{
				throw std::system_error(errno, std::system_category());
			}
			const auto listen_filters = _listen_filters;

			_watching_single_file = is_file(path);

			const T watch_path = [this, &path]() {
				if (_watching_single_file)
				{
					const auto parsed_path = split_directory_and_file(path);
					_filename = parsed_path.filename;
					return parsed_path.directory;
				}
				else
				{
					return path;
				}
			}();

			std::cout << "watch path: " << watch_path << std::endl;
			const auto watch = inotify_add_watch(folder, watch_path.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE);
			if (watch < 0) 
			{
				throw std::system_error(errno, std::system_category());
			}
			return { folder, watch };
		}

		void monitor_directory() 
		{
			std::vector<char> buffer(_buffer_size);

			while (_destory == false) 
			{
				const auto length = read(_directory.folder, static_cast<void*>(buffer.data()), buffer.size());
				if (length > 0) 
				{
					int i = 0;
					std::vector<std::pair<T, Event>> parsed_information;
					while (i < length) 
					{
						struct inotify_event *event = (struct inotify_event *) &buffer[i];
						if (event->len) 
						{
							const string_type changed_file{ event->name };
							if (pass_filter(changed_file))
							{
								if (event->mask & IN_CREATE) 
								{
									parsed_information.emplace_back(T{ changed_file }, Event::added);
								}
								else if (event->mask & IN_DELETE) 
								{
									parsed_information.emplace_back(T{ changed_file }, Event::removed);
								}
								else if (event->mask & IN_MODIFY) 
								{
									parsed_information.emplace_back(T{ changed_file }, Event::modified);
								}
							}
						}
						i += event_size + event->len;
					}
					//dispatch callbacks
					{
						std::lock_guard<std::mutex> lock(_callback_mutex);
						_callback_information.insert(_callback_information.end(), parsed_information.begin(), parsed_information.end());
					}
					cv.notify_all();
				}
			}
		}
#endif // __unix__

		void callback_thread()
		{
			while (_destory == false) {
				std::unique_lock<std::mutex> lock(_callback_mutex);
				if (_callback_information.empty() && _destory == false) {
					cv.wait(lock, [this] { return _callback_information.size() > 0 || _destory; });
				}
				decltype(_callback_information) callback_information = {};
				std::swap(callback_information, _callback_information);
				lock.unlock();

				for (const auto& file : callback_information) {
					if (_callback) {
						try
						{
							_callback(file.first, file.second);
						}
						catch (const std::exception&)
						{
						}
					}
				}
			}
		}
	};
}
#endif