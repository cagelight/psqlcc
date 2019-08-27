#pragma once

#include <asterales/strop.hh>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>

struct pg_result;

namespace postgres {
	
	typedef std::runtime_error exception;
	
	struct stringable {
		stringable (std::string const & str) : str(str) {}
		stringable (std::string && str) : str(std::forward<std::string &&>(str)) {}
		stringable (char const * str) : str(str) {}
		template <typename T> stringable(T const & v) : str {std::to_string(v)} {}
		std::string str;
		inline operator std::string const & () const { return str; }
	};

	typedef std::initializer_list<stringable> params_list;
	
	typedef int64_t bigint_t;
	typedef int32_t int_t;
	
	struct value {
		inline value(char const * str) : str(str) {}
		std::string const & string() const { return str; }
		int_t integer() const { return strtol(str.c_str(), nullptr, 10); }
		bigint_t bigint() const { return strtoll(str.c_str(), nullptr, 10); }
		bool boolean() const { return (str[0] == 't'); }
		inline operator std::string const & () const { return str; }
		inline operator int_t () const { return integer(); }
		inline operator bigint_t () const { return bigint(); }
		inline operator bool () const { return boolean(); }
	protected:
		std::string str;
	};

	struct result {
		result();
		result(pg_result *);
		result(result const &) = delete;
		result(result && other);
		~result();
		
		int num_fields() const;
		int num_rows() const;
		value get_value(int row, int field) const;
		std::vector<value> get_row(int row) const;
		std::string get_error() const;
		bool cmd_ok() const;
		bool tuples_ok() const;
		
		result & operator = (result const & other) = delete;
		result & operator = (result && other);
		inline value operator () (int row, int field) const { return get_value(row, field); }
		
		struct const_iterator {
			result const * parent;
			int index = 0;
			const_iterator() = default;
			const_iterator(result const * parent, int index = 0) : parent (parent), index(index) {}
			std::vector<value> operator * () const { return parent->get_row(index); }
			const_iterator & operator ++ () { index++; return *this; }
			bool operator != (const_iterator const & other) { return parent != other.parent && index != other.index; }
		};
		
		const_iterator begin() const { return const_iterator {this, 0}; }
		const_iterator end() const { return const_iterator {this, this->num_rows()}; }

	private:
		struct private_data;
		std::unique_ptr<private_data> data;
	};

	struct cmd_result : public result {
		cmd_result(result && r) : result(std::forward<result &&>(r)) {}
		operator bool () { return cmd_ok(); }
	};
	
	struct connection {
		connection() = delete;
		connection(std::string const & dbname);
		connection(connection const &) = delete;
		connection(connection &&) = delete;
		~connection();
		
		result exec(std::string const & cmd);
		result exec_params(std::string const & cmd, params_list);
		inline cmd_result cmd(std::string const & cmd) { return exec(cmd); }
		inline cmd_result cmd_params(std::string const & cmd, params_list params) { return exec_params(cmd, std::move(params)); }
		bool ok();
		
	private:
		struct private_data;
		std::unique_ptr<private_data> data;
	};

	struct pool {
		
		struct pool_con {
			pool_con() = delete;
			pool_con(std::string dbname, std::mutex & cvm, std::condition_variable & cv) : con(dbname), in_use(false), cvm(cvm), cv(cv) {}
			~pool_con() = default;
			void reset() {
				in_use.clear();
				cv.notify_one();
			}
			connection con;
			std::atomic_flag in_use;
		private:
			std::mutex & cvm;
			std::condition_variable & cv;
		};
		
		struct conview {
			conview() = delete;
			conview(std::shared_ptr<pool_con> ptr) : ptr(ptr) {}
			conview(conview const&& other) = delete;
			conview(conview && other) : ptr(other.ptr), in_transaction_block(other.in_transaction_block) {
				other.ptr.reset();
				other.in_transaction_block = false;
			}
			~conview() {
				if (!ptr) return;
				if (in_transaction_block) cmd("ROLLBACK");
				ptr->reset();
			}
			
			inline bool ok() { return ptr && ptr->con.ok(); }
			inline result exec(std::string const & cmd) { return ptr->con.exec(cmd); }
			inline result exec_params(std::string const & cmd, params_list params) { return ptr->con.exec_params(cmd, std::move(params)); }
			inline cmd_result cmd(std::string const & cmd) { return ptr->con.cmd(cmd); }
			inline cmd_result cmd_params(std::string const & cmd, params_list params) { return ptr->con.cmd_params(cmd, std::move(params)); }
			inline void begin() { cmd("BEGIN"); in_transaction_block = true; }
			inline void commit() { cmd("COMMIT"); in_transaction_block = false; }
			inline void rollback() { cmd("ROLLBACK"); in_transaction_block = false; }
		private:
			std::shared_ptr<pool_con> ptr;
			bool in_transaction_block = false;
		};
		
		pool(std::string const & dbname, unsigned int num_cons = std::thread::hardware_concurrency());
		~pool() = default;
		
		inline bool ok() { return ok_; }
		conview try_acquire(); // check ok() before using conview, ALWAYS
		conview acquire(); // blocks until available, no need to check ok()
	private:
		bool ok_ = false;
		std::mutex cvm;
		std::condition_variable cv;
		std::vector<std::shared_ptr<pool_con>> cons;
	};

}
