#pragma once

template<typename Template>
class Singleton {
public:
	static Template& GetInstance() {
		static Template Instance{};
		return Instance;
	}

protected:
	Singleton() { }
	~Singleton() { }

	Singleton(const Singleton&) = delete;
	Singleton& operator=(const Singleton&) = delete;

	Singleton(Singleton&&) = delete;
	Singleton& operator=(Singleton&&) = delete;
};
