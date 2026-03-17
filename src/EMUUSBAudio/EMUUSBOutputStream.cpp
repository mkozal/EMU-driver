//
//  EMUUSBOutputStream.cpp
//  EMUUSBAudio
//
//  Created by Wouter Pasman on 31/12/14.
//  Copyright (c) 2014 com.emu. All rights reserved.
//

#include "EMUUSBOutputStream.h"
#include "EMUUSBLogging.h"
#include "EMUUSBAudioCommon.h"


IOReturn   EMUUSBOutputStream::init() {
    ReturnIf(initialized, kIOReturnStillOpen);
    
    IOReturn res = StreamInfo::init();
    ReturnIf(res != kIOReturnSuccess, res);
    
    shouldStop = 0;

    for (int i = 0; i < NUMBER_FRAMES; i++) {
        theWrapSubDescriptors[i][0] = OSTypeAlloc (IOSubMemoryDescriptor);
        theWrapSubDescriptors[i][1] = OSTypeAlloc (IOSubMemoryDescriptor);
        theWrapRangeDescriptors[i] = NULL;
        if (!theWrapSubDescriptors[i][0] || !theWrapSubDescriptors[i][1]) return kIOReturnNoMemory;
    }

    
    frameSizeQueue = NULL;
    initialized=true;
    
    return kIOReturnSuccess;
}

IOReturn EMUUSBOutputStream::start(FrameSizeQueue *frameQueue,UInt64 startUsbFrame, UInt32 frameSamples) {
    debugIOLogW("EMUUSBOutputStream::start at %lld",mach_absolute_time());
    ReturnIf(shouldStop, kIOReturnStillOpen); // still in closedown! Cancel start.
    ReturnIfFail(StreamInfo::reset());
    ReturnIfFail(StreamInfo::start(startUsbFrame));

    currentFrameList = 0;

    stockSamplesInFrame = frameSamples;

    frameSizeQueue = frameQueue;
    
    started = true; // must be true before we start writing to USB.
    
    for (UInt32 frameListNum = currentFrameList; frameListNum < numUSBFrameListsToQueue; frameListNum++) {
		//debugIOLog("write frame list %d at %lld",frameListNum,mach_absolute_time());
        // FIXME handle failures?
		writeFrameList(frameListNum);
	}


    return kIOReturnSuccess;
}


IOReturn EMUUSBOutputStream::stop() {
    debugIOLogC("EMUUSBOutputStream::stop");
    
    ReturnIf(!started, kIOReturnNotOpen);
    started = false;
    if (shouldStop == 0) {
        shouldStop=1;
    }
    return kIOReturnSuccess;
}


void EMUUSBOutputStream::queueTerminated() {
    started = false;
    if (shouldStop==0) {
        shouldStop = 2; // 1 stopped; force full stop
    } else {
        shouldStop ++;
    }

    debugIOLogC("writeHandler: now stopped %d of %d",shouldStop-1,numUSBFrameListsToQueue);
    
    // shouldStop counter starts at 1 when stop is initiated and increases 1 for
    // every completed framelist
    if (shouldStop == numUSBFrameListsToQueue + 1) {
        debugIOLog("All playback streams stopped");
        started = false;
        shouldStop = 0;
        notifyClosed();
    }

    
}

void EMUUSBOutputStream::free() {
    if (started) {
        doLog("EMUUSBOutputStream::free BUG free() called without stop()");
    }
    debugIOLogC("EMUUSBOutputStream::free");
    for (int i = 0; i < NUMBER_FRAMES; i++) {
        if (theWrapRangeDescriptors[i]) theWrapRangeDescriptors[i]->release();
        if (theWrapSubDescriptors[i][0]) theWrapSubDescriptors[i][0]->release();
        if (theWrapSubDescriptors[i][1]) theWrapSubDescriptors[i][1]->release();
        theWrapRangeDescriptors[i] = NULL;
    }
    initialized=false;
}


IOReturn EMUUSBOutputStream::writeFrameList (UInt32 frameListNum) {
    ReturnIf(!started, kIOReturnNoDevice);
    ReturnIf(!pipe,kIOReturnNoDevice);
    
    //debugIOLogW("EMUUSBAudioEngine::writeFrameList %d",frameListNum);
	IOReturn	result = kIOReturnError;
    result = PrepareWriteFrameList (frameListNum);
    ReturnIf (kIOReturnSuccess != result, result);


    UInt64  frameNr = getNextFrameNr();
    if (needTimeStamps) {
        result = pipe->Write (theWrapRangeDescriptors[frameListNum],frameNr,numUSBFramesPerList,
                              &usbIsocFrames[frameListNum * numUSBFramesPerList], &usbCompletion[frameListNum], 1);
        needTimeStamps = FALSE;
    } else {
        result = pipe->Write(bufferDescriptors[frameListNum],frameNr,numUSBFramesPerList,
                             &usbIsocFrames[frameListNum * numUSBFramesPerList], &usbCompletion[frameListNum], 1);
    }
    debugIOLogW("WRITE framenr %lld at %lld",frameNr, mach_absolute_time());
	return result;
}

void EMUUSBOutputStream::writeCompletedStatic (void * object, void * parameter, IOReturn result, LowLatencyIsocFrame * pFrames) {
    if (object) {
        ((EMUUSBOutputStream *) object)->writeCompleted(parameter, result, pFrames);
    }
}

void EMUUSBOutputStream::writeCompleted (void * parameter, IOReturn result, LowLatencyIsocFrame * pFrames) {
    if (!streamInterface) return;
    
    if (kIOReturnSuccess != result && kIOReturnAborted != result) {
        doLog("** writeCompleted bad result %x\n",result);
        return;
    }
    
    static int writeLogCount = 0;
    if (writeLogCount++ % 5000 == 0) {
        doLog("EMUUSBOutputStream::writeCompleted success (call #%d)\n", writeLogCount);
    }
    
    if (shouldStop) {
        queueTerminated();
        return;
    }
    
    
    // FIXME: simplistic, flawed locking mechanism
    // Seems we don't need a lock here anyway as this is only called as callback.
    if (inWriteCompletion)
    {
        debugIOLog("*** BUG already in write completion!");
		return;
    }
    inWriteCompletion = TRUE;
    
    
    // FIXME use '%' instead of this weird multiply trick
    // FIXME why update list already here?
    // FIXME remove this weird construction, it basically just recovers
    // the original frame number in a very unintelligeble and possibly buggy way.
    // We get orig number only because numUSBFrameLists == actual #framelists and
    // the completion order is the same as the Write order. But recovering the real
    // list number is critical as we would restart an already running Write if we
    // get this wrong.
    currentFrameList = (currentFrameList + 1) * (currentFrameList < (numUSBFrameLists - 1));
    
    // FIXME use % operator
    UInt32	frameListToWrite = (currentFrameList - 1) + numUSBFrameListsToQueue;
    frameListToWrite -= numUSBFrameLists * (frameListToWrite >= numUSBFrameLists);
    IOReturn writeResult = writeFrameList (frameListToWrite);
    if (writeResult != kIOReturnSuccess) {
            // #29 if write fails, we can't keep running
            debugIOLog("PIPE write error :%x. Stopping OutputStream.", writeResult);
            handleUSBError(writeResult);
            queueTerminated();
    }
    
	inWriteCompletion = FALSE;
	return;
}


IOReturn EMUUSBOutputStream::PrepareWriteFrameList (UInt32 listNr) {
    //debugIOLogW ("+EMUUSBAudioEngine::PrepareWriteFrameList");
    ReturnIf(!started, kIOReturnNoDevice);
    
    //ReturnIf(frameSizeQueue.available() < numUSBFramesPerList, kIOReturnUnderrun);
    ReturnIf(!audioStream, kIOReturnNoDevice);
    
	UInt32			sampleBufferSize = audioStream->getSampleBufferSize() ;
    ReturnIf(!sampleBufferSize, kIOReturnNoMemory);
    
	IOReturn		result = kIOReturnError;// default
    UInt32			thisFrameListSize = 0;
    UInt32			thisFrameSize = 0;
    UInt32			firstFrame = listNr * numUSBFramesPerList;
    UInt32			numBytesToBufferEnd = sampleBufferSize - previouslyPreparedBufferOffset;
    UInt32			lastPreparedByte = previouslyPreparedBufferOffset;
    bool			haveWrapped = false;
    
    
    // Set to number of bytes from the 0 wrap, 0 if this buffer didn't wrap
    usbCompletion[listNr].set((void *)this, (LowLatencyCompletionAction)writeCompletedStatic, 0);
    // usbCompletion[listNr].target = (void *)this;
    // usbCompletion[listNr].action = (LowLatencyCompletionAction)writeCompletedStatic;
    // usbCompletion[listNr].parameter = 0;
    
    
    
    //debugIOLogW("PrepareWriteFrameList stockSamplesInFrame %d numUSBFramesPerList %d", stockSamplesInFrame, numUSBFramesPerList);
    for (UInt32 n = 0; n < numUSBFramesPerList; n++) {
        
        if (frameSizeQueue->pop(&thisFrameSize) != kIOReturnSuccess) {
            static int emptyQueueLogCount = 0;
            if (emptyQueueLogCount++ % 1000 == 0) {
                doLog("frameSizeQueue empty, guessing some queue size. (Occurred %d times)\n", emptyQueueLogCount);
            }
            thisFrameSize = (stockSamplesInFrame+1)  * multFactor ;
        }
        
        if (thisFrameSize >= numBytesToBufferEnd) {
            //debugIOLog("write wrap in usbframe %lld list %d byte %d",nextUsableUsbFrameNr,n,numBytesToBufferEnd);
            lastPreparedByte = thisFrameSize - numBytesToBufferEnd;
            usbCompletion[listNr].parameter = (void *)(UInt64)(((n + 1) << 16) | lastPreparedByte);
            theWrapSubDescriptors[listNr][0]->initSubRange (usbBufferDescriptor, previouslyPreparedBufferOffset, sampleBufferSize - previouslyPreparedBufferOffset, kIODirectionInOut);
            numBytesToBufferEnd = sampleBufferSize - lastPreparedByte;// reset
            haveWrapped = true;
        } else {
            thisFrameListSize += thisFrameSize;
            lastPreparedByte += thisFrameSize;
            numBytesToBufferEnd -= thisFrameSize;
            //			debugIOLogC("no param");
        }
        usbIsocFrames[firstFrame + n].set(-1, thisFrameSize, 0, 0);
        //  usbIsocFrames[firstFrame + n].frStatus = -1;
        //  usbIsocFrames[firstFrame + n].frActCount = 0;
        //  usbIsocFrames[firstFrame + n].frReqCount = thisFrameSize;
    }
    //debugIOLogC("Done with the numUSBFrames loop");
    //debugIOLogW("num actual data frames in list %d",numUSBFramesPerList - contiguousZeroes);
    if (haveWrapped) {
        needTimeStamps = TRUE;
        theWrapSubDescriptors[listNr][0]->initSubRange (usbBufferDescriptor, previouslyPreparedBufferOffset, sampleBufferSize - previouslyPreparedBufferOffset, kIODirectionInOut);
        theWrapSubDescriptors[listNr][1]->initSubRange (usbBufferDescriptor, 0, lastPreparedByte, kIODirectionInOut);
        
        if (NULL != theWrapRangeDescriptors[listNr]) {
            theWrapRangeDescriptors[listNr]->release ();
        }
        
        theWrapRangeDescriptors[listNr] = IOMultiMemoryDescriptor::withDescriptors ((IOMemoryDescriptor **)theWrapSubDescriptors[listNr], 2, kIODirectionInOut, true);
    } else {
        bufferDescriptors[listNr]->initSubRange (usbBufferDescriptor, previouslyPreparedBufferOffset, thisFrameListSize, kIODirectionInOut);
        FailIf (NULL == bufferDescriptors[listNr], Exit);
    }
    
    //debugIOLogW("EMUUSBAudioEngine::PrepareWriteFrameList %d size %d %s", listNr, thisFrameSize,haveWrapped?"-":"wrap");
    //debugIOLogW("PrepareWriteFrameList: lastPrepareFrame %d safeToEraseTo %d",lastPreparedByte / multFactor, safeToEraseTo / multFactor);
    
    previouslyPreparedBufferOffset = lastPreparedByte;
    result = kIOReturnSuccess;
    
Exit:
	return result;
}

