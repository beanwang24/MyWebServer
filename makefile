server: main.cpp ./code/timer/min_heap_timer.h ./code/timer/min_heap_timer.cpp ./code/threadpool/threadpool.h ./code/http/http_conn.cpp ./code/http/http_conn.h ./code/lock/locker.h ./code/log/log.cpp ./code/log/log.h ./code/log/block_queue.h ./code/CGImysql/sql_connection_pool.cpp ./code/CGImysql/sql_connection_pool.h
	g++ -o server main.cpp ./code/timer/min_heap_timer.h ./code/timer/min_heap_timer.cpp ./code/threadpool/threadpool.h ./code/http/http_conn.cpp ./code/http/http_conn.h ./code/lock/locker.h ./code/log/log.cpp ./code/log/log.h ./code/CGImysql/sql_connection_pool.cpp ./code/CGImysql/sql_connection_pool.h -lpthread -lmysqlclient


clean:
	rm  -r server
