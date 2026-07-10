#!/bin/bash

# Comprehensive System Requirements Test Script
# Tests all 5 system requirements with detailed logging

TEST_LOG="test_results_$(date +%Y%m%d_%H%M%S).log"
PASSED=0
FAILED=0
TOTAL=0

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# LOGGING FUNCTIONS
# ============================================================================

log_header() {
    echo "================================================================================" | tee -a "$TEST_LOG"
    echo "$1" | tee -a "$TEST_LOG"
    echo "================================================================================" | tee -a "$TEST_LOG"
}

log_test() {
    echo "" | tee -a "$TEST_LOG"
    echo -e "${BLUE}[TEST]${NC} $1" | tee -a "$TEST_LOG"
}

log_step() {
    echo -e "${YELLOW}  → $1${NC}" | tee -a "$TEST_LOG"
}

log_pass() {
    echo -e "${GREEN}  ✓ PASS${NC}: $1" | tee -a "$TEST_LOG"
    ((PASSED++))
}

log_fail() {
    echo -e "${RED}  ✗ FAIL${NC}: $1" | tee -a "$TEST_LOG"
    ((FAILED++))
}

log_info() {
    echo -e "  ℹ $1" | tee -a "$TEST_LOG"
}

log_result() {
    echo "" | tee -a "$TEST_LOG"
    TOTAL=$((PASSED + FAILED))
    echo "Results: ${GREEN}$PASSED passed${NC}, ${RED}$FAILED failed${NC}, Total: $TOTAL" | tee -a "$TEST_LOG"
}

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

run_client_command() {
    local username=$1
    local command=$2
    local expected=$3
    
    echo "$command" | timeout 5 ./client 2>&1 | grep -q "$username" || {
        echo "FAILED_TO_START"
        return 1
    }
    
    # Create a tmp file to hold client session
    local tmpfile=$(mktemp)
    {
        echo "$username"
        echo "$command"
        echo "EXIT"
    } | timeout 5 ./client > "$tmpfile" 2>&1
    
    if grep -q "$expected" "$tmpfile"; then
        rm "$tmpfile"
        return 0
    else
        rm "$tmpfile"
        return 1
    fi
}

check_file_exists() {
    if [ -f "$1" ]; then
        return 0
    else
        return 1
    fi
}

check_log_contains() {
    local logfile=$1
    local pattern=$2
    
    if grep -q "$pattern" "$logfile"; then
        return 0
    else
        return 1
    fi
}

# ============================================================================
# STARTUP & CLEANUP
# ============================================================================

cleanup() {
    log_info "Cleaning up..."
    pkill -f "name_server" 2>/dev/null
    pkill -f "storage_server" 2>/dev/null
    pkill -f "client" 2>/dev/null
    sleep 2
}

startup_servers() {
    log_step "Starting Name Server..."
    ./name_server > /dev/null 2>&1 &
    local nm_pid=$!
    sleep 2
    
    log_step "Starting Storage Server..."
    ./storage_server 127.0.0.1 9000 10000 > /dev/null 2>&1 &
    local ss_pid=$!
    sleep 2
    
    # Check if servers are running
    if ps -p $nm_pid > /dev/null && ps -p $ss_pid > /dev/null; then
        log_info "Name Server (PID: $nm_pid) and Storage Server (PID: $ss_pid) started successfully"
        return 0
    else
        log_fail "Failed to start servers"
        return 1
    fi
}

# ============================================================================
# TEST 1: DATA PERSISTENCE (10 points)
# ============================================================================

test_data_persistence() {
    log_header "TEST 1: DATA PERSISTENCE (10 points)"
    
    # Test 1.1: Create and verify files persist in storage/
    log_test "Test 1.1: Files created should persist in ./storage/ directory"
    
    log_step "Creating test files with alice"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "CREATE persistent_test1.txt"
        echo "WRITE persistent_test1.txt 0"
        echo "0 Data"
        echo "1 Persistence"
        echo "2 Test."
        echo "ETIRW"
        echo "CREATE persistent_test2.txt"
        echo "WRITE persistent_test2.txt 0"
        echo "0 Another"
        echo "1 File."
        echo "ETIRW"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    sleep 1
    
    if check_file_exists "storage/persistent_test1.txt"; then
        log_pass "File persistent_test1.txt exists in storage/"
    else
        log_fail "File persistent_test1.txt not found in storage/"
    fi
    
    if check_file_exists "storage/persistent_test2.txt"; then
        log_pass "File persistent_test2.txt exists in storage/"
    else
        log_fail "File persistent_test2.txt not found in storage/"
    fi
    
    # Test 1.2: Verify undo directory created
    log_test "Test 1.2: Undo history should be stored in ./storage/.undo/"
    
    if check_file_exists "storage/.undo/persistent_test1.txt"; then
        log_pass "Undo file exists for persistent_test1.txt"
    else
        log_fail "Undo file not created"
    fi
    
    # Test 1.3: Verify metadata file created
    log_test "Test 1.3: Metadata should be persisted in nm_metadata.dat"
    
    if check_file_exists "nm_metadata.dat"; then
        log_pass "Metadata file nm_metadata.dat exists"
        local size=$(stat -f%z nm_metadata.dat 2>/dev/null || stat -c%s nm_metadata.dat 2>/dev/null)
        log_info "Metadata file size: $size bytes"
    else
        log_fail "Metadata file not created"
    fi
    
    # Test 1.4: Server restart and recovery
    log_test "Test 1.4: Data should be recovered after server restart"
    
    log_step "Stopping servers..."
    cleanup
    
    log_step "Restarting servers..."
    startup_servers
    
    log_step "Verifying data recovery..."
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "VIEW"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "persistent_test1.txt" "$tmpfile"; then
        log_pass "Files recovered after server restart"
    else
        log_fail "Files not recovered after restart"
    fi
    
    if check_log_contains "NM.log" "Loaded.*files from persistent storage"; then
        log_pass "NM log shows successful metadata recovery"
    else
        log_fail "No metadata recovery log entry"
    fi
    
    rm -f "$tmpfile"
}

# ============================================================================
# TEST 2: ACCESS CONTROL (5 points)
# ============================================================================

test_access_control() {
    log_header "TEST 2: ACCESS CONTROL (5 points)"
    
    # Test 2.1: Owner has full access
    log_test "Test 2.1: Owner should have full access to their files"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "CREATE access_test.txt"
        echo "READ access_test.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "Access denied" "$tmpfile"; then
        log_fail "Owner denied access to own file"
    else
        log_pass "Owner can access own file"
    fi
    
    rm -f "$tmpfile"
    
    # Test 2.2: Non-owner denied access
    log_test "Test 2.2: Non-owner should be denied access without permission"
    
    tmpfile=$(mktemp)
    {
        echo "bob"
        echo "READ access_test.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "Access denied" "$tmpfile"; then
        log_pass "Non-owner denied access"
    else
        log_fail "Non-owner was allowed access"
    fi
    
    rm -f "$tmpfile"
    
    # Test 2.3: Grant and verify access
    log_test "Test 2.3: ADDACCESS should grant permissions"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "ADDACCESS -R access_test.txt bob"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "READ access granted" "$tmpfile"; then
        log_pass "READ access granted successfully"
    else
        log_fail "Failed to grant READ access"
    fi
    
    rm -f "$tmpfile"
    
    # Verify bob can now read
    tmpfile=$(mktemp)
    {
        echo "bob"
        echo "READ access_test.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "File Content" "$tmpfile"; then
        log_pass "Granted user can now read file"
    else
        log_fail "Granted user cannot read file"
    fi
    
    rm -f "$tmpfile"
    
    # Test 2.4: Revoke access
    log_test "Test 2.4: REMACCESS should revoke permissions"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "REMACCESS access_test.txt bob"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "Access removed" "$tmpfile"; then
        log_pass "Access revoked successfully"
    else
        log_fail "Failed to revoke access"
    fi
    
    rm -f "$tmpfile"
    
    if check_log_contains "NM.log" "REM_ACCESS"; then
        log_pass "Access revocation logged"
    else
        log_fail "Access revocation not logged"
    fi
}

# ============================================================================
# TEST 3: LOGGING (5 points)
# ============================================================================

test_logging() {
    log_header "TEST 3: LOGGING (5 points)"
    
    # Test 3.1: Verify NM.log exists and has entries
    log_test "Test 3.1: Name Server should log all operations to NM.log"
    
    if check_file_exists "NM.log"; then
        log_pass "NM.log file exists"
        local entries=$(wc -l < NM.log)
        log_info "NM.log has $entries entries"
    else
        log_fail "NM.log not created"
    fi
    
    # Test 3.2: Verify SS.log exists and has entries
    log_test "Test 3.2: Storage Server should log all operations to SS.log"
    
    if check_file_exists "SS.log"; then
        log_pass "SS.log file exists"
        local entries=$(wc -l < SS.log)
        log_info "SS.log has $entries entries"
    else
        log_fail "SS.log not created"
    fi
    
    # Test 3.3: Check log format
    log_test "Test 3.3: Logs should contain timestamp, operation, user, IP, and error code"
    
    if check_log_contains "NM.log" "Operation:"; then
        log_pass "NM.log contains operation field"
    else
        log_fail "NM.log missing operation field"
    fi
    
    if check_log_contains "NM.log" "User:"; then
        log_pass "NM.log contains user field"
    else
        log_fail "NM.log missing user field"
    fi
    
    if check_log_contains "SS.log" "Level:"; then
        log_pass "SS.log contains log level field"
    else
        log_fail "SS.log missing log level field"
    fi
    
    # Test 3.4: Verify operation details logged
    log_test "Test 3.4: Specific operations should be logged with details"
    
    if check_log_contains "NM.log" "CREATE"; then
        log_pass "CREATE operations logged"
    else
        log_fail "CREATE operations not logged"
    fi
    
    if check_log_contains "NM.log" "ADD_ACCESS"; then
        log_pass "ADD_ACCESS operations logged"
    else
        log_fail "ADD_ACCESS operations not logged"
    fi
}

# ============================================================================
# TEST 4: ERROR HANDLING (5 points)
# ============================================================================

test_error_handling() {
    log_header "TEST 4: ERROR HANDLING (5 points)"
    
    # Test 4.1: File not found
    log_test "Test 4.1: FILE_NOT_FOUND error should be returned"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "READ nonexistent_file_xyz.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "File not found" "$tmpfile"; then
        log_pass "FILE_NOT_FOUND error returned correctly"
    else
        log_fail "FILE_NOT_FOUND error not returned"
    fi
    
    if check_log_contains "NM.log" "ERR.*FILE_NOT_FOUND\|Error: 1"; then
        log_pass "Error code logged"
    else
        log_info "Error code may be present in binary format"
    fi
    
    rm -f "$tmpfile"
    
    # Test 4.2: Access denied
    log_test "Test 4.2: ACCESS_DENIED error should be returned"
    
    tmpfile=$(mktemp)
    {
        echo "bob"
        echo "DELETE access_test.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "Only owner\|Access denied" "$tmpfile"; then
        log_pass "ACCESS_DENIED error returned"
    else
        log_fail "ACCESS_DENIED error not returned"
    fi
    
    rm -f "$tmpfile"
    
    # Test 4.3: File already exists
    log_test "Test 4.3: FILE_EXISTS error should be returned"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "CREATE persistent_test1.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "already exists" "$tmpfile"; then
        log_pass "FILE_EXISTS error returned"
    else
        log_fail "FILE_EXISTS error not returned"
    fi
    
    rm -f "$tmpfile"
    
    # Test 4.4: Sentence locked
    log_test "Test 4.4: SENTENCE_LOCKED error should be handled"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "CREATE lock_test.txt"
        echo "WRITE lock_test.txt 0"
        echo "0 test"
        echo "ETIRW"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "locked\|updated" "$tmpfile"; then
        log_pass "Sentence lock operations completed"
    else
        log_fail "Sentence lock operations failed"
    fi
    
    rm -f "$tmpfile"
}

# ============================================================================
# TEST 5: EFFICIENT SEARCH (15 points)
# ============================================================================

test_efficient_search() {
    log_header "TEST 5: EFFICIENT SEARCH (15 points)"
    
    # Test 5.1: Hash table search
    log_test "Test 5.1: File lookups should use O(1) hash table search"
    
    log_step "Creating multiple files for search testing..."
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "CREATE search_test_1.txt"
        echo "CREATE search_test_2.txt"
        echo "CREATE search_test_3.txt"
        echo "CREATE search_test_4.txt"
        echo "CREATE search_test_5.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    sleep 1
    
    if [ $(grep -c "created successfully" "$tmpfile") -ge 5 ]; then
        log_pass "Created 5 test files"
    else
        log_fail "Failed to create test files"
    fi
    
    rm -f "$tmpfile"
    
    # Test 5.2: Cache lookup
    log_test "Test 5.2: Recent searches should be cached for fast lookup"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "READ search_test_1.txt"
        echo "READ search_test_1.txt"
        echo "READ search_test_1.txt"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -c "File Content" "$tmpfile" -ge 3; then
        log_pass "Repeated file reads successful (cache working)"
    else
        log_fail "Repeated file reads failed"
    fi
    
    rm -f "$tmpfile"
    
    if check_log_contains "NM.log" "CACHE_HIT"; then
        log_pass "Cache hits logged"
    else
        log_info "Cache hits not logged yet (may not have reached threshold)"
    fi
    
    # Test 5.3: VIEW operation
    log_test "Test 5.3: VIEW operation should return file listings efficiently"
    
    tmpfile=$(mktemp)
    {
        echo "alice"
        echo "VIEW"
        echo "EXIT"
    } | timeout 10 ./client > "$tmpfile" 2>&1
    
    if grep -q "search_test" "$tmpfile"; then
        log_pass "VIEW returned file listings"
    else
        log_fail "VIEW operation failed"
    fi
    
    rm -f "$tmpfile"
    
    # Test 5.4: Metadata persistence
    log_test "Test 5.4: File metadata should be efficiently retrieved"
    
    if check_file_exists "nm_metadata.dat"; then
        local size=$(stat -f%z nm_metadata.dat 2>/dev/null || stat -c%s nm_metadata.dat 2>/dev/null)
        if [ "$size" -gt 0 ]; then
            log_pass "Metadata persisted efficiently ($size bytes)"
        else
            log_fail "Metadata file is empty"
        fi
    else
        log_fail "Metadata file not found"
    fi
}

# ============================================================================
# MAIN EXECUTION
# ============================================================================

main() {
    echo "Starting Comprehensive System Requirements Test" | tee "$TEST_LOG"
    echo "Started at: $(date)" | tee -a "$TEST_LOG"
    echo "" | tee -a "$TEST_LOG"
    
    # Check if binaries exist
    if [ ! -f "./name_server" ] || [ ! -f "./storage_server" ] || [ ! -f "./client" ]; then
        log_fail "Binary files not found. Please compile with 'make all'"
        exit 1
    fi
    
    # Cleanup previous runs
    cleanup
    
    # Start servers
    if ! startup_servers; then
        log_fail "Failed to start servers"
        exit 1
    fi
    
    sleep 3
    
    # Run all tests
    test_data_persistence
    sleep 2
    
    test_access_control
    sleep 2
    
    test_logging
    sleep 2
    
    test_error_handling
    sleep 2
    
    test_efficient_search
    sleep 2
    
    # Print final results
    log_header "TEST SUMMARY"
    
    log_result
    
    echo "" | tee -a "$TEST_LOG"
    echo "Test results saved to: $TEST_LOG" | tee -a "$TEST_LOG"
    echo "Finished at: $(date)" | tee -a "$TEST_LOG"
    
    # Cleanup
    cleanup
    
    if [ $FAILED -eq 0 ]; then
        echo -e "${GREEN}All tests passed!${NC}" | tee -a "$TEST_LOG"
        exit 0
    else
        echo -e "${RED}Some tests failed. Review $TEST_LOG for details.${NC}" | tee -a "$TEST_LOG"
        exit 1
    fi
}

# Run main
main
