/**
 * @author See Contributors.txt for code contributors and overview of BadgerDB.
 *
 * @section LICENSE
 * Copyright (c) 2012 Database Group, Computer Sciences Department, University of Wisconsin-Madison.
 */

#include <memory>
#include <iostream>
#include "buffer.h"
#include "exceptions/buffer_exceeded_exception.h"
#include "exceptions/page_not_pinned_exception.h"
#include "exceptions/page_pinned_exception.h"
#include "exceptions/bad_buffer_exception.h"
#include "exceptions/hash_not_found_exception.h"

namespace badgerdb { 

BufMgr::BufMgr(std::uint32_t bufs)
	: numBufs(bufs) {
	bufDescTable = new BufDesc[bufs];

  for (FrameId i = 0; i < bufs; i++) 
  {
  	bufDescTable[i].frameNo = i;
  	bufDescTable[i].valid = false;
  }

  bufPool = new Page[bufs];

	int htsize = ((((int) (bufs * 1.2))*2)/2)+1;
  hashTable = new BufHashTbl (htsize);  // allocate the buffer hash table

  clockHand = bufs - 1;
}

	/**
   	 * Destructor of BufMgr class
	 */
BufMgr::~BufMgr() {
	// Flushes out all dirty pages
	for (FrameId i=0; i<numBufs; i++)
		if(bufDescTable[i].valid == true && bufDescTable[i].dirty == true)
			flushFile(bufDescTable[i].file);

	// Deallocates the buffer pool and the BufDesc table.
	delete[] bufDescTable;
	delete[] bufPool;
	delete hashTable;
}

	/**
   * Advance clock to next frame in the buffer pool
	 */
void BufMgr::advanceClock()
{
	clockHand = (clockHand+1) % numBufs;
}

	/**
	 * Allocate a free frame.
	 *
	 * @param frame   	Frame reference, frame ID of allocated frame returned via this variable
	 * @throws BufferExceededException If no such buffer is found which can be allocated
	 */
void BufMgr::allocBuf(FrameId & frame)
{
	// Remember the start point and pass.
	// All frame is pinned if two passed be made.
	FrameId flag = clockHand;
	int pass = 0;
	while(pass < 2) {
		advanceClock();
		if (clockHand == flag) pass++;
		// If this frame is unused, return this page.
		if (bufDescTable[clockHand].valid == false) {
			frame = clockHand;
			return;
		}
		// If this page is recently used, clear the ref bit, continue.
		if (bufDescTable[clockHand].refbit == true) {
			bufDescTable[clockHand].refbit = false;
			continue;
		}
		// If this page is pinned, continue to next.
		if (bufDescTable[clockHand].pinCnt > 0) {
			continue;
		}
		// This frame is selected, clean this frame.
		if (bufDescTable[clockHand].dirty == true) {
			bufDescTable[clockHand].file->writePage(bufPool[clockHand]);
		}
		// Remove the appropriate entry from the hash table.
		hashTable->remove(bufDescTable[clockHand].file,bufDescTable[clockHand].pageNo);
		bufDescTable[clockHand].Clear();
		frame = clockHand;
		return;
	}
	// If the buffer is full.
	throw BufferExceededException();
}

	/**
	 * Reads the given page from the file into a frame and returns the pointer to page.
	 * If the requested page is already present in the buffer pool pointer to that frame is returned
	 * otherwise a new frame is allocated from the buffer pool for reading the page.
	 *
	 * @param file   	File object
	 * @param PageNo  Page number in the file to be read
	 * @param page  	Reference to page pointer. Used to fetch the Page object in which requested page from file is read in.
	 */
void BufMgr::readPage(File* file, const PageId pageNo, Page*& page)
{
	FrameId tmpFrameId;
	try{
		// Page is in the buffer pool.
		hashTable->lookup(file, pageNo, tmpFrameId);
		bufDescTable[tmpFrameId].pinCnt ++;
	}catch (HashNotFoundException e){
		// Page is not in the buffer pool.
		// Allocate a buffer frame. Read the page from disk.
		// Insert the page into the hashtable. Set the frame.
		allocBuf(tmpFrameId);
		bufPool[tmpFrameId] = file->readPage(pageNo);
		hashTable->insert(file, pageNo, tmpFrameId);
		bufDescTable[tmpFrameId].Set(file, pageNo);
	}
	bufDescTable[tmpFrameId].refbit = true;
	// Return a pointer to the frame containing the page.
	page = &bufPool[tmpFrameId];
}

	/**
	 * Unpin a page from memory since it is no longer required for it to remain in memory.
	 *
	 * @param file   	File object
	 * @param PageNo  Page number
	 * @param dirty		True if the page to be unpinned needs to be marked dirty
   * @throws  PageNotPinnedException If the page is not already pinned
	 */
void BufMgr::unPinPage(File* file, const PageId pageNo, const bool dirty) 
{
	FrameId tmpFrameId;

	try{
		hashTable->lookup(file, pageNo, tmpFrameId);
	}catch (HashNotFoundException e){
		// Does nothing if page is not found in the hash table lookup.
		return;
	}

	// Throws PAGENOTPINNED if the pin count is already 0.
	if(bufDescTable[tmpFrameId].pinCnt == 0)
		throw PageNotPinnedException(file->filename(), pageNo, tmpFrameId);
	bufDescTable[tmpFrameId].pinCnt--;
	if(dirty) bufDescTable[tmpFrameId].dirty = true;	//if dirty == true, sets the dirty bit.
}

	/**
	 * Writes out all dirty pages of the file to disk.
	 * All the frames assigned to the file need to be unpinned from buffer pool before this function can be successfully called.
	 * Otherwise Error returned.
	 *
	 * @param file   	File object
   * @throws  PagePinnedException If any page of the file is pinned in the buffer pool
   * @throws BadBufferException If any frame allocated to the file is found to be invalid
	 */
void BufMgr::flushFile(const File* file) 
{
	//Throws PagePinnedException if some page of the file is pinned.
	//Throws BadBufferException if an invalid page belonging to the file is encountered.
	for (FrameId i=0;i<numBufs;i++)
		if(bufDescTable[i].file == file) {
			if (bufDescTable[i].valid == false)
				throw BadBufferException(i, bufDescTable[i].dirty, bufDescTable[i].valid, bufDescTable[i].refbit);
			if (bufDescTable[i].pinCnt > 0)
				throw PagePinnedException(file->filename(), bufDescTable[i].pageNo, i);
		}

	// Scan bufTable for pages belonging to the file.
	// If the page is dirty, call file->writePage() to flush the page to disk
	// and then set the dirty bit for the page to false.
	// Remove the page from the hashtable (whether the page is clean or dirty.
	// Invoke the Clear() method of BufDesc for the page frame.
	for (FrameId i=0; i<numBufs; i++)
		if(bufDescTable[i].file == file){
			if(bufDescTable[i].dirty == true){
				bufDescTable[i].file->writePage(bufPool[i]);
				bufDescTable[i].dirty = false;
			}
			hashTable->remove(file,bufDescTable[i].pageNo);
			bufDescTable[i].Clear();
		}
}

	/**
	 * Allocates a new, empty page in the file and returns the Page object.
	 * The newly allocated page is also assigned a frame in the buffer pool.
	 *
	 * @param file   	File object
	 * @param PageNo  Page number. The number assigned to the page in the file is returned via this reference.
	 * @param page  	Reference to page pointer. The newly allocated in-memory Page object is returned via this reference.
	 */
void BufMgr::allocPage(File* file, PageId &pageNo, Page*& page) 
{
	FrameId tmpFrameId;
	// Allocate an empty page in the specified file and obtain a buffer pool.
	PageId NewPage = file->allocatePage().page_number();
	allocBuf(tmpFrameId);

	// Set the hash table and frame.
	bufPool[tmpFrameId] = file->readPage(NewPage);
	hashTable->insert(file, NewPage, tmpFrameId);
	bufDescTable[tmpFrameId].Set(file, NewPage);

	pageNo = NewPage;
	page = &bufPool[tmpFrameId];
}

	/**
	 * Delete page from file and also from buffer pool if present.
	 * Since the page is entirely deleted from file, its unnecessary to see if the page is dirty.
	 *
	 * @param file   	File object
	 * @param PageNo  Page number
	 */
void BufMgr::disposePage(File* file, const PageId PageNo)
{
	FrameId tmpFrameId;
	try{
		// Makes sure that if the page to be deleted is allocated a frame in the buffer pool, that frame
		// is freed and correspondingly entry from hash table is also removed.
		hashTable->lookup(file, PageNo, tmpFrameId);
		bufDescTable[tmpFrameId].Clear();
		hashTable->remove(file,PageNo);
	}catch (HashNotFoundException e){
		// Page not in buffer pool, just delete.
	}

	file->deletePage(PageNo);

}

void BufMgr::printSelf(void) 
{
  BufDesc* tmpbuf;
	int validFrames = 0;
  
  for (std::uint32_t i = 0; i < numBufs; i++)
	{
  	tmpbuf = &(bufDescTable[i]);
		std::cout << "FrameNo:" << i << " ";
		tmpbuf->Print();

  	if (tmpbuf->valid == true)
    	validFrames++;
  }

	std::cout << "Total Number of Valid Frames:" << validFrames << "\n";
}

}
