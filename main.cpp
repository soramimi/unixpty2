
#include <QCoreApplication>
#include <QDebug>
#include <QMutex>
#include <QThread>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <QWaitCondition>
#include <termios.h>
#include <unistd.h>

class UnixPtyProcess2 : public QThread {
private:
	int ptm = -1;
public:
	QMutex mutex;
	QWaitCondition waiter;
	std::deque<char> output_queue;
protected:
	void run()
	{
		char *argv[2] = {"/bin/bash", nullptr};
		try {
			ptm = posix_openpt(O_RDWR);
			if (ptm == -1) throw QString::asprintf("posix_openpt error: %s\n", strerror(errno));
			if (grantpt(ptm) == -1) throw QString::asprintf("grantpt error: %s\n", strerror(errno));
			if (unlockpt(ptm) == -1) throw QString::asprintf("unlockpt error: %s\n", strerror(errno));

			int pid1 = fork();
			if (pid1 < 0) {
				throw QString::asprintf("fork error: %s\n", strerror(errno));
			} else if (pid1 == 0) { // child
				if (setsid() == (pid_t) -1) {
					throw QString::asprintf("setsid() error: %s\n", strerror(errno));
				}
				char *ptr = ptsname(ptm);
				if (ptr == nullptr) {
					throw QString::asprintf("ptsname error: %s\n", strerror(errno));
				}
				int pts = open(ptr, O_RDWR);
				if (pts < 0) {
					throw QString::asprintf("open of slave failed: %a\n", strerror(errno));
				}
				close(ptm);

				dup2(pts, STDIN_FILENO);
				dup2(pts, STDOUT_FILENO);
				dup2(pts, STDERR_FILENO);

				if (execve(*argv, argv , nullptr) == -1) {
					throw QString::asprintf("execve error: %s\n", strerror(errno));
				}
				exit(1);
			} else { // parent
				waiter.wakeAll();
				if (dup2(ptm, STDIN_FILENO) != STDIN_FILENO) {
					throw QString::asprintf("dup2 of parent failed");
				}
				while (1) {
					char buf[512];
					int n = read(ptm, buf, sizeof(buf));
					if (n <= 0) {
						break;
					}
					{
						QMutexLocker lock(&mutex);
						output_queue.insert(output_queue.end(), buf, buf + n);
					}
				}
			}
		} catch (QString const &e) {
			qDebug() << e;
		}
	}
public:
	void writeInput(char const *ptr, int len)
	{
		int r = write(ptm, ptr, len);
		(void)r;
	}
	int readOutput(char *ptr, int len)
	{
		QMutexLocker lock(&mutex);
		int n = output_queue.size();
		if (n > len) {
			n = len;
		}
		if (n > 0) {
			auto it = output_queue.begin();
			std::copy(it, it + n, ptr);
			output_queue.erase(it, it + n);
		}
		return n;
	}
};

int main(int argc, char **argv)
{
	QCoreApplication a(argc, argv);

	UnixPtyProcess2 t;
	t.start();
	{
		QMutexLocker lock(&t.mutex);
		t.waiter.wait(&t.mutex);
	}
	t.writeInput("ls\r", 3);
	t.writeInput("exit\r", 5);
	t.wait();

	puts("---done---");
	int n = t.output_queue.size();
	for (int i = 0; i < n; i++) {
		char tmp[16];
		int l = t.readOutput(tmp, sizeof(tmp));
		fwrite(tmp, 1, l, stdout);
	}
	return 0;
}

