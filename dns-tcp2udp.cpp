#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <string>

#include "dns-tcp2udp.hpp"

using namespace std;

int main(int argc, char *argv[]) {
	list<string> args;

	while (argc-- > 0)
		args.push_back((argv++)[0]);

	string name = args.front();
	args.pop_front();

	if (args.size() < 2) {
		cout << "Usage: " << name << " <dest ip> <listen ip> [listen ip]...\n";
		exit(EXIT_FAILURE);
	}

	string dest = args.front();
	args.pop_front();

	Proxy(name, args, dest).mainLoop();
	exit(EXIT_SUCCESS);
}
