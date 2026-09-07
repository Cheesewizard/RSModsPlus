#include "../../DLL/Audio/MlServiceLauncher.cpp"

namespace Midi::Digitech::WhammyDT { void AutoTuning(int, float) {} }
namespace Midi::Digitech::BassWhammy { void AutoTuningAndTrueTuning(int, float) {} }
namespace Midi::Digitech::WhammyFour { void AutoTuningAndTrueTuning(int, float) {} }
namespace Midi::Digitech::WhammyFive { void AutoTuningAndTrueTuning(int, float) {} }
namespace Midi::Software { void AutoTuning(int, float) {} }

int main(int argumentCount, char** arguments)
{
	const auto directory = std::filesystem::path(arguments[0]).parent_path();
	if (argumentCount > 1 && std::string(arguments[1]) == "--ml-service")
	{
		std::ofstream(directory / "starts.txt", std::ios::app) << GetCurrentProcessId() << std::endl;
		while (!std::filesystem::exists(directory / "exit")) Sleep(20);
		return 17;
	}
	const auto deadline = GetTickCount64() + 30000;
	while (GetTickCount64() < deadline && !std::filesystem::exists(directory / "stop"))
	{
		MlServiceLauncher::EnsureStarted();
		Sleep(25);
	}
	MlServiceLauncher::Shutdown();
	return 0;
}
