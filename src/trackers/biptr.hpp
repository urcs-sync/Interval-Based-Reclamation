/*

Copyright 2017 University of Rochester

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. 

*/


#ifndef BIPTR_HPP
#define BIPTR_HPP

template<class T> struct FatPtr{
	T* ptr;
	uint64_t tag;
	FatPtr(): ptr(nullptr), tag(0){}
	FatPtr(T* p, uint64_t t) : ptr(p), tag(t) {}
} __attribute__(( __aligned__(16)));

template<class T> class biptr{
private:
	std::atomic<uint64_t>* birth_before;
	std::atomic<T*>* p;
	//FatPtr typed 128 bit record
	std::atomic<FatPtr<T>> fat_ptr;

public:
	static RangeTracker<T>* range_tracker;

	biptr<T>(){
		birth_before = (std::atomic<uint64_t>*) ((char*) &(fat_ptr) + sizeof(T*));
		p = (std::atomic<T*>*)((char*) &(fat_ptr));
	}
	biptr<T>(T* obj) {
		std::atomic_init(&fat_ptr, FatPtr<T>(obj, get_birth_epoch(obj)));
		birth_before = (std::atomic<uint64_t>*) ((char*) &(fat_ptr) + sizeof(T*));
		p = (std::atomic<T*>*)((char*) &(fat_ptr));

	}
	biptr<T>(biptr<T> &other) {
		std::atomic_init(&fat_ptr, FatPtr<T>(other.ptr(), other.birth()));
		birth_before = (std::atomic<uint64_t>*) ((char*) &(fat_ptr) + sizeof(T*));
		p = (std::atomic<T*>*)((char*) &(fat_ptr));
	}

	static void set_tracker(RangeTracker<T>* tracker){
		range_tracker = tracker;
	}

	inline uint64_t get_epoch(){
		return range_tracker->get_epoch();
	}

	inline uint64_t birth(){
		// return range_tracker->type == WCAS? 
		// 	fat_ptr.load_tag() : birth_before->load(std::memory_order_acquire);
		return birth_before->load(std::memory_order_acquire);
	}
	inline T* ptr() {
		// return range_tracker->type == WCAS?
		// 	fat_ptr.load_ptr() : p->load(std::memory_order_acquire);
		return p->load(std::memory_order_acquire);
	}
	inline T* load(){
		return ptr();
	}
	inline uint64_t get_birth_epoch(T* obj){
		T* ptr = (T*) ((size_t)obj & 0xfffffffffffffffc); //in case the last bit is toggled.
		return (ptr)? RangeTracker<T>::read_birth(ptr) : 0;
	}

	inline bool CAS(T* &ori, T* obj, std::memory_order morder){
		if (range_tracker->type != WCAS){
			uint64_t e_ori = birth_before->load(std::memory_order_acquire);
			uint64_t birth_epoch = get_birth_epoch(obj);
			switch (range_tracker->type){
				case LF:
					while(true){
						if (e_ori < birth_epoch){
							if (birth_before->compare_exchange_weak(e_ori, 
								birth_epoch, morder, std::memory_order_relaxed)){
								break;
							}
						} else {
							break;
						}
					}
					break;
				case FAA:
					if (e_ori < birth_epoch){
						birth_before->fetch_add(birth_epoch - e_ori, std::memory_order_acq_rel);
					}
					break;
				default:
					break;
			}
			return p->compare_exchange_strong(ori, obj, morder,
				std::memory_order_relaxed);
		} else { // WCAS
			FatPtr<T> old_value(ori, birth_before->load());
			FatPtr<T> new_value(obj, get_birth_epoch(obj));
			return fat_ptr.compare_exchange_strong(old_value, new_value);
		}
	}

	inline bool CAS(T* &ori, T* obj){
		return CAS(ori, obj, std::memory_order_release);
	}

	inline T* protect_and_fetch_ptr(){
		while(true){
			range_tracker->update_reserve(birth());
			T* ret = this->ptr();
			//if (range_tracker->validate(get_birth_epoch(ret))){
			if (range_tracker->validate(birth())){
				return (ret);
			}
		}
	}

	inline biptr<T>& store(T* obj){
		if (range_tracker->type != WCAS){
			this->birth_before->store(get_birth_epoch(obj), std::memory_order_relaxed);
			this->p->store(obj, std::memory_order_relaxed);
		} else {
			FatPtr<T> old_value(p->load(), birth_before->load());
			FatPtr<T> new_value(obj, get_birth_epoch(obj));
			while(!this->fat_ptr.compare_exchange_strong(old_value, new_value));
		}
		return *this;
	}

	//Please note that operator = is not thread safe due to separate update of p and birth_before
	//The WCAS version is thread safe.
	inline biptr<T>& operator = (const biptr<T> &other){
		if (this != &other){
			//while(!this->CAS(other.ptr()));
			if (range_tracker->type != WCAS){
				this->birth_before->store(other.birth(), std::memory_order_relaxed);
				this->p->store(other.ptr(), std::memory_order_relaxed);
			} else {
				FatPtr<T> old_value(p->load(), birth_before->load());
				FatPtr<T> new_value(other.ptr(), other.birth());
				while(!this->fat_ptr.compare_exchange_strong(old_value, new_value));
			}
		}
		return *this;
	}
	inline biptr<T>& operator = (T* obj){
		return store(obj);
	}
	inline bool operator == (void* obj){
		return (this->ptr() == obj);
	}
	inline bool operator != (void* obj){
		return (this->ptr() != obj);
	}
	inline T* operator -> (){
		return protect_and_fetch_ptr();
	}
	inline T& operator * (){
		return *protect_and_fetch_ptr();
	}
	inline explicit operator bool(){
		return (this->ptr()!=NULL);
	}

};

#endif