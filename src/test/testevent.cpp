#include <doctest/doctest.h>
#include "../event.h"
#include <thread>
#include <atomic>
#include <memory>

using namespace std;

TEST_CASE("Events - test events")
{
	// Add two events, call WaitForSignal() from another thread, check if events executed
	int counter = 0;
	auto func = [&counter]() {
		counter++;
	};

	CEvents events;
	events.AddEventFunction(func);
	events.AddEventFunction(func);

	std::atomic<bool> done{ false };

	auto eventThreadFunc = [&done, &events]() {
		events.WaitForSignal();

		IEvent* ev = events.GetNextEvent();

		CHECK(ev);
		ev->Execute();
		ev = events.GetNextEvent();

		CHECK(ev);
		ev->Execute();
		ev = events.GetNextEvent();

		CHECK(!ev);

		done = true;
	};

	thread t(eventThreadFunc);

	// wait for a thread to end
	this_thread::sleep_for(chrono::milliseconds(200));

	// check if thread is done
	CHECK(done == true);

	t.join();

	CHECK(counter == 2);
}

TEST_CASE("Events - release captured resources")
{
	SUBCASE("after execution")
	{
		CEvents events;
		auto resource = make_shared<int>(1);
		weak_ptr<int> weakResource = resource;
		events.AddEventFunction([resource]() {});
		resource.reset();

		IEvent* event = events.GetNextEvent();
		REQUIRE(event != nullptr);
		event->Execute();
		CHECK(weakResource.expired());
	}

	SUBCASE("when the queue is destroyed")
	{
		weak_ptr<int> weakResource;
		{
			CEvents events;
			auto resource = make_shared<int>(1);
			weakResource = resource;
			events.AddEventFunction([resource]() {});
		}
		CHECK(weakResource.expired());
	}

	SUBCASE("when socket events are cancelled")
	{
		CEvents events;
		auto resource = make_shared<int>(1);
		weak_ptr<int> weakResource = resource;
		IExtendedSocket* socket = reinterpret_cast<IExtendedSocket*>(1);
		events.AddEventTCPPacket(socket, [resource]() {});
		resource.reset();

		events.RemoveEventsBySocket(socket);
		CHECK(weakResource.expired());
	}
}
