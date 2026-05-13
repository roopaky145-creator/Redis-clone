import redis
import threading
import time

# --- Benchmark Configuration ---
TOTAL_REQUESTS = 100_000
CONCURRENT_CLIENTS = 50
requests_per_client = TOTAL_REQUESTS // CONCURRENT_CLIENTS

# Connect to your C++ server for ping
client = redis.Redis(host='localhost', port=6379, decode_responses=True)

def run_workload():
    """Worker thread firing SET requests as fast as possible"""
    thread_client = redis.Redis(host='localhost', port=6379, decode_responses=True)
    thread_id = threading.get_ident()
    for i in range(requests_per_client):
        # We write unique keys to test the LRU eviction and Map hashing
        thread_client.set(f"bench:{thread_id}:{i}", "performance_test_payload")

print(f"Starting Benchmark: {TOTAL_REQUESTS} SETs across {CONCURRENT_CLIENTS} threads...")

# Wait for the server to be ready
client.ping()

start_time = time.time()

threads = []
for _ in range(CONCURRENT_CLIENTS):
    t = threading.Thread(target=run_workload)
    threads.append(t)
    t.start()

for t in threads:
    t.join()

end_time = time.time()
duration = end_time - start_time
qps = TOTAL_REQUESTS / duration

print("-" * 30)
print(f"Benchmark Complete!")
print(f"Time Taken : {duration:.3f} seconds")
print(f"Throughput : {qps:,.0f} QPS (Queries Per Second)")
print("-" * 30)
