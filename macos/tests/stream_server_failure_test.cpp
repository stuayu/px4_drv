#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "command.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"
#include "runtime_path.hpp"
#include "stream_server.hpp"

namespace px4 {
void pipe_server_cleanup();
}

namespace {

class FakeReceiver final : public px4::ReceiverBase {
public:
	FakeReceiver() : ReceiverBase(0) {}
	int Open() override { return 0; }
	void Close() override {}
	int CheckLock(bool &locked) override { locked = true; return 0; }
	int SetLnbVoltage(std::int32_t) override { return 0; }
	int SetCapture(bool) override { return 0; }
	int ReadStat(px4::command::StatType, std::int32_t &value) override
	{
		value = 0;
		return 0;
	}

protected:
	int SetFrequency() override { return 0; }
	int SetStreamId() override { return 0; }
};

bool ConnectLocal(const std::string &path, int &fd)
{
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	int no_sigpipe = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (path.size() >= sizeof(address.sun_path))
		return false;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
			return true;
		if (errno != ECONNREFUSED && errno != ENOENT)
			return false;
		usleep(10000);
	}
	return false;
}

bool SendAll(int fd, const void *data, std::size_t size)
{
	const auto *bytes = static_cast<const std::uint8_t *>(data);
	std::size_t sent = 0;
	while (sent < size) {
		const ssize_t result = send(fd, bytes + sent, size - sent, 0);
		if (result <= 0)
			return false;
		sent += static_cast<std::size_t>(result);
	}
	return true;
}

bool WaitForDisconnect(int fd)
{
	pollfd poll_fd{};
	poll_fd.fd = fd;
	poll_fd.events = POLLIN | POLLHUP | POLLERR;
	if (poll(&poll_fd, 1, 2000) <= 0)
		return false;
	char byte = 0;
	const ssize_t result = recv(fd, &byte, sizeof(byte), 0);
	return result == 0 || (result < 0 &&
		(errno == ECONNRESET || errno == ENOTCONN));
}

} // namespace

int main()
{
	char runtime_template[] = "/tmp/px4-stream-failure-XXXXXX";
	char *runtime_dir = mkdtemp(runtime_template);
	if (!runtime_dir) {
		std::perror("mkdtemp");
		return 1;
	}
	setenv("PX4_RUNTIME_DIR", runtime_dir, 1);

	px4::ReceiverManager receiver_manager;
	FakeReceiver receiver;
	px4::command::ReceiverInfo info{};
	if (!receiver_manager.Register(info, &receiver)) {
		std::fprintf(stderr, "[DEBUG-bd22] receiver register failed\n");
		return 1;
	}
	px4::command::ReceiverInfo key{}, opened{};
	key.index = -1;
	std::uint32_t data_id = 0;
	if (receiver_manager.SearchAndOpen(key, opened, data_id) != &receiver) {
		std::fprintf(stderr, "[DEBUG-bd22] receiver open failed\n");
		return 1;
	}

	auto stream_buf = receiver.GetStreamBuffer();
	if (!stream_buf->Alloc(188 * 4)) {
		std::fprintf(stderr, "[DEBUG-bd22] stream buffer allocation failed\n");
		return 1;
	}
	stream_buf->Start();
	stream_buf->NotifyStreamFailure();

	px4::StreamServer server(receiver_manager);
	if (!server.Start()) {
		std::fprintf(stderr, "[DEBUG-bd22] stream server start failed: %d (%s)\n",
			server.GetError().value(), server.GetError().message().c_str());
		return 1;
	}
	int client = -1;
	const std::string path = px4::runtime_path("px4_data_pipe.sock");
	if (!ConnectLocal(path, client)) {
		std::fprintf(stderr, "could not connect to the data pipe\n");
		server.Stop();
		return 1;
	}

	px4::command::DataCmd command{};
	command.cmd = px4::command::DataCmdCode::SET_DATA_ID;
	command.data_id = data_id;
	const bool sent = SendAll(client, &command, sizeof(command));
	const bool disconnected = sent && WaitForDisconnect(client);
	close(client);
	const bool stopped = server.Stop();
	stream_buf->Stop();
	receiver.Close();
	receiver_manager.Unregister(&receiver);
	px4::pipe_server_cleanup();
	unsetenv("PX4_RUNTIME_DIR");
	rmdir(runtime_dir);

	if (!disconnected || !stopped) {
		std::fprintf(stderr, "stream failure did not close the data pipe (sent=%d closed=%d stopped=%d)\n",
			sent, disconnected, stopped);
		return 1;
	}

	std::puts("stream_server_failure_test: passed");
	return 0;
}
