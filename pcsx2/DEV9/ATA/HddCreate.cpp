/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include <fstream>
#include "HddCreate.h"

void HddCreate::Start()
{
#ifndef PCSX2_CORE
	//This can be called from the EE Core thread
	//ensure that UI creation/deletaion is done on main thread
	if (!wxIsMainThread())
	{
		wxTheApp->CallAfter([&] { Start(); });
		//Block until done
		std::unique_lock competedLock(completedMutex);
		completedCV.wait(competedLock, [&] { return completed; });
		return;
	}

#endif

	int reqMiB = (neededSize + ((1024 * 1024) - 1)) / (1024 * 1024);

#ifndef PCSX2_CORE
	//This creates a modeless dialog
	progressDialog = new wxProgressDialog(_("Creating HDD file"), _("Creating HDD file"), reqMiB, nullptr, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
#endif

	fileThread = std::thread(&HddCreate::WriteImage, this, filePath, neededSize);

	//This code was written for a modal dialog, however wxProgressDialog is modeless only
	//The idea was block here in a ShowModal() call, and have the worker thread update the UI
	//via CallAfter()

	//Instead, loop here to update UI
	wxString msg;
	int currentSize;
	while ((currentSize = written.load()) != reqMiB && !errored.load())
	{
		msg.Printf(_("%i / %i MiB"), written.load(), reqMiB);

#ifndef PCSX2_CORE
		if (!progressDialog->Update(currentSize, msg))
			canceled.store(true);
#endif

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	fileThread.join();

	if (errored.load())
	{
#ifndef PCSX2_CORE
		wxMessageDialog dialog(nullptr, _("Failed to create HDD file"), _("Info"), wxOK);
		dialog.ShowModal();
#endif
	}

#ifndef PCSX2_CORE
	delete progressDialog;
#endif
	//Signal calling thread to resume
	{
		std::lock_guard ioSignallock(completedMutex);
		completed = true;
	}
	completedCV.notify_all();
}

void HddCreate::WriteImage(fs::path hddPath, u64 reqSizeBytes)
{
	constexpr int buffsize = 4 * 1024;
	u8 buff[buffsize] = {0}; //4kb

	if (fs::exists(hddPath))
	{
		SetError();
		return;
	}

	std::fstream newImage = fs::fstream(hddPath, std::ios::out | std::ios::binary);

	if (newImage.fail())
	{
		SetError();
		return;
	}

	//Size file
	newImage.seekp(reqSizeBytes - 1, std::ios::beg);
	const char zero = 0;
	newImage.write(&zero, 1);

	if (newImage.fail())
	{
		newImage.close();
		fs::remove(filePath);
		SetError();
		return;
	}

	lastUpdate = std::chrono::steady_clock::now();

	newImage.seekp(0, std::ios::beg);

	//Round up
	const s32 reqMiB = (reqSizeBytes + ((1024 * 1024) - 1)) / (1024 * 1024);
	for (s32 iMiB = 0; iMiB < reqMiB; iMiB++)
	{
		//Round down
		const s32 req4Kib = std::min<s32>(1024, (reqSizeBytes / 1024) - (u64)iMiB * 1024) / 4;
		for (s32 i4kb = 0; i4kb < req4Kib; i4kb++)
		{
			newImage.write((char*)buff, buffsize);
			if (newImage.fail())
			{
				newImage.close();
				fs::remove(filePath);
				SetError();
				return;
			}
		}

		if (req4Kib != 256)
		{
			const s32 remainingBytes = reqSizeBytes - (((u64)iMiB) * (1024 * 1024) + req4Kib * 4096);
			newImage.write((char*)buff, remainingBytes);
			if (newImage.fail())
			{
				newImage.close();
				fs::remove(filePath);
				SetError();
				return;
			}
		}

		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() >= 100 || (iMiB + 1) == reqMiB)
		{
			lastUpdate = now;
			SetFileProgress(iMiB + 1);
		}
		if (canceled.load())
		{
			newImage.close();
			fs::remove(filePath);
			SetError();
			return;
		}
	}
	newImage.flush();
	newImage.close();
}

void HddCreate::SetFileProgress(int currentSize)
{
	written.store(currentSize);
}

void HddCreate::SetError()
{
	errored.store(true);
}
