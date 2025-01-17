@echo off

REM detect paths
set msbuild_cmd=msbuild.exe
set devenv_cmd=devenv.exe
where /q devenv.exe
if not %errorlevel%==0 set devenv_cmd="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\devenv.exe"
where /q msbuild.exe
if not %errorlevel%==0 set msbuild_cmd="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"

:begin
	cls

	echo Wut?
	echo ===============================
	echo   1. Exit
	echo   2. Create project
	echo   3. Build release
	echo   4. Run Studio
	echo   5. Open in VS
	echo   6. Create bundle
	echo   7. Pull latest from Github
	echo   8. Open chat
	echo   9. 3rd party
	echo   A. Plugins
	echo   B. Download Godot Engine
	echo ===============================
	choice /C 123456789AB /N /M "Your choice:"
	echo.

	if %errorlevel%==1 goto :EOF
	if %errorlevel%==2 call :create_project
	if %errorlevel%==3 call :build
	if %errorlevel%==4 call :run_studio
	if %errorlevel%==5 call :open_in_vs
	if %errorlevel%==6 call :create_bundle
	if %errorlevel%==7 call :git_pull
	if %errorlevel%==8 call :open_gitter
	if %errorlevel%==9 call :third_party
	if %errorlevel%==10 call :plugins
	if %errorlevel%==11 call :download_godot
goto :begin

:plugins 
	cls
	echo Wut?
	echo ===============================
	echo  1. Go back
	echo  2. Maps
	echo  3. Shader editor
	echo ===============================
	choice /C 1234567 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :map_plugin
	if %errorlevel%==3 call :shader_editor_plugin
	pause
goto :plugins


:shader_editor_plugin
	if not exist ..\plugins mkdir ..\plugins
	pushd ..\plugins
	if not exist shader_editor (
		git.exe clone https://github.com/nem0/lumixengine_shader_editor.git shader_editor
	) else (
		cd shader_editor
		git pull
	)
	popd
exit /B 0

:map_plugin
	if not exist ..\plugins mkdir ..\plugins
	pushd ..\plugins
	if not exist maps (
		git.exe clone https://github.com/nem0/lumixengine_maps.git maps
	) else (
		cd maps
		git pull
	)
	popd
exit /B 0

:third_party 
	REM we should use specific 3rd party revision
	cls
	echo Wut2?
	echo ===============================
	echo  1. Go back
	echo  2. Download, build and deploy all
	echo  3. NVTT
	echo  4. Recast navigation
	echo  5. CMFT
	echo  6. PhysX
	echo  7. LuaJIT
	echo  8. FreeType2
	echo ===============================
	choice /C 12345678 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :all_3rdparty
	if %errorlevel%==3 call :nvtt
	if %errorlevel%==4 call :recast
	if %errorlevel%==5 call :cmft
	if %errorlevel%==6 call :physx
	if %errorlevel%==7 call :luajit
	if %errorlevel%==8 call :freetype
goto :third_party

:all_3rdparty
	call :download_physx
	call :download_nvtt
	call :download_cmft
	call :download_recast
	call :download_luajit
	call :download_freetype
	
	call :build_physx
	call :build_nvtt
	call :build_cmft
	call :build_recast
	call :build_luajit
	call :build_freetype
	
	call :deploy_physx
	call :deploy_nvtt
	call :deploy_cmft
	call :deploy_recast
	call :deploy_luajit
	call :deploy_freetype
	pause

exit /B 0

:luajit
	cls
	echo LuaJIT
	echo ===============================
	echo  1. Go back
	echo  2. Download
	echo  3. Build
	echo  4. Deploy
	echo ===============================
	choice /C 1234 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :download_luajit
	if %errorlevel%==3 call :build_luajit
	if %errorlevel%==4 call :deploy_luajit
	pause
goto :luajit

:deploy_luajit
	del /Q ..\external\luajit\lib\win64_vs2017\release\*
	del /Q ..\external\luajit\include\*
	copy 3rdparty\luajit\src\lua51.lib ..\external\luajit\lib\win64_vs2017\release\
	copy 3rdparty\luajit\src\luajit.lib ..\external\luajit\lib\win64_vs2017\release\
	copy 3rdparty\luajit\src\lauxlib.h ..\external\luajit\include
	copy 3rdparty\luajit\src\lua.h ..\external\luajit\include
	copy 3rdparty\luajit\src\lua.hpp ..\external\luajit\include
	copy 3rdparty\luajit\src\luaconf.h ..\external\luajit\include
	copy 3rdparty\luajit\src\luajit.h ..\external\luajit\include
	copy 3rdparty\luajit\src\lualib.h ..\external\luajit\include
exit /B 0

:build_luajit
	pushd 3rdparty\luajit\src
	call msvcbuild.bat
	popd
exit /B 0

:download_luajit
	if not exist 3rdparty mkdir 3rdparty
	cd 3rdparty
	if not exist luajit (
		git.exe clone https://github.com/LuaJIT/LuaJIT.git luajit
	) else (
		cd luajit
		git pull
		cd ..
	)
	cd ..
exit /B 0

:freetype
	cls
	echo FreeType2
	echo ===============================
	echo  1. Go back
	echo  2. Download
	echo  3. Build
	echo  4. Deploy
	echo  5. Open in VS
	echo ===============================
	choice /C 12345 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :download_freetype
	if %errorlevel%==3 call :build_freetype
	if %errorlevel%==4 call :deploy_freetype
	if %errorlevel%==5 start "" %devenv_cmd% "3rdparty\freetype\builds\windows\vs2010\freetype.sln"
	pause
goto :freetype

:build_freetype
	%msbuild_cmd% 3rdparty\freetype\builds\windows\vc2010\freetype.sln /p:Configuration="Release Static" /p:Platform=x64
exit /B 0

:deploy_freetype
echo %CD%
	del /Q ..\external\freetype\lib\win64_vs2017\release\*
	copy "3rdparty\freetype\objs\x64\Release Static\freetype.lib" ..\external\freetype\lib\win64_vs2017\release\
	copy "3rdparty\freetype\objs\x64\Release Static\freetype.pdb" ..\external\freetype\lib\win64_vs2017\release\
	del /Q ..\external\freetype\include\*
	xcopy /E /Y "3rdparty\freetype\include\*" ..\external\freetype\include\
exit /B 0

:physx
	cls
	echo PhysX
	echo ===============================
	echo  1. Go back
	echo  2. Download
	echo  3. Build
	echo  4. Deploy
	echo  5. Open in VS
	echo ===============================
	choice /C 12345 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :download_physx
	if %errorlevel%==3 call :build_physx
	if %errorlevel%==4 call :deploy_physx
	if %errorlevel%==5 start "" %devenv_cmd% "3rdparty\PhysX\physx\compiler\vc15win64\PhysXSDK.sln"
	pause
goto :physx

:deploy_physx
	REM lib
	del /Q ..\external\physx\lib\vs2017\win64\release\*
	copy 3rdparty\PhysX\physx\compiler\vc15win64\sdk_source_bin\FastXml.dir\release\FastXml.lib ..\external\physx\lib\vs2017\win64\release\FastXml_static_64.lib 
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\LowLevelAABB_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\LowLevelDynamics_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\LowLevel_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCharacterKinematic_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCommon_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCooking_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXExtensions_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXFoundation_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXPvdSDK_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXTask_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXVehicle_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysX_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\SceneQuery_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\SimulationController_static_64.lib ..\external\physx\lib\vs2017\win64\release\
	REM include
	for /D %%e in (..\external\physx\include\*) do rmdir /Q /S %%e
	del /Q ..\external\physx\include\*
	xcopy /E /Y 3rdparty\PhysX\physx\include\* ..\external\physx\include\
	xcopy /E /Y 3rdparty\PhysX\pxshared\include\* ..\external\physx\include\
	REM dll
	del /Q ..\external\physx\dll\vs2017\win64\release\*
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCommon_64.dll ..\external\physx\dll\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCooking_64.dll ..\external\physx\dll\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXFoundation_64.dll ..\external\physx\dll\vs2017\win64\release\
	copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysX_64.dll ..\external\physx\dll\vs2017\win64\release\
exit /B 0

:build_physx
	cd 3rdparty\PhysX\physx
	call generate_projects.bat lumix_vc15win64
	%msbuild_cmd% "compiler\vc15win64\PhysXSDK.sln" /p:Configuration=Release /p:Platform=x64
	cd ..\..\..\
exit /B 0

:recast
	cls
	echo Recast ^& Detour
	echo ===============================
	echo  1. Go back
	echo  2. Download
	echo  3. Build
	echo  4. Deploy
	echo  5. Open in VS
	echo ===============================
	choice /C 12345 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :download_recast
	if %errorlevel%==3 call :build_recast
	if %errorlevel%==4 call :deploy_recast
	if %errorlevel%==5 start "" %devenv_cmd% "3rdparty\recast\_project\RecastDetour.sln"
	pause
goto :recast

:deploy_recast
	del /Q ..\external\recast\include\*
	del /Q ..\external\recast\src\*
	copy 3rdparty\recast\Recast\Include\* ..\external\recast\include\
	copy 3rdparty\recast\Detour\Source\* ..\external\recast\src\
	copy 3rdparty\recast\Detour\Include\* ..\external\recast\include\
	copy 3rdparty\recast\DetourCrowd\Include\* ..\external\recast\include\
	copy 3rdparty\recast\DetourCrowd\Source\* ..\external\recast\src\
	copy 3rdparty\recast\DebugUtils\Include\* ..\external\recast\include\
	copy 3rdparty\recast\_build\Recast.lib ..\external\recast\lib\win64_vs2017\release\recast.lib
	copy 3rdparty\recast\_build\Recast.pdb ..\external\recast\lib\win64_vs2017\release\recast.pdb
exit /B 0

:build_recast
	genie.exe --file=recastnavigation.lua vs2019
	%msbuild_cmd% 3rdparty\recast\_project\RecastDetour.sln /p:Configuration=Release /p:Platform=x64
exit /B 0

:cmft 
	cls
	echo CMFT
	echo ===============================
	echo  1. Go back
	echo  2. Download
	echo  3. Build
	echo  4. Deploy
	echo  5. Open in VS
	echo ===============================
	choice /C 12345 /N /M "Your choice:"
	echo.
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :download_cmft
	if %errorlevel%==3 call :build_cmft
	if %errorlevel%==4 call :deploy_cmft
	if %errorlevel%==5 start "" %devenv_cmd% "3rdparty\cmft\_projects\vs2019\cmft.sln"
	pause
goto :cmft

:build_cmft
	cd 3rdparty\cmft\scripts
	copy ..\..\..\genie.exe genie.exe
	genie.exe --file=main.lua vs2019
	del genie.exe
	cd ..\..\..
	%msbuild_cmd% 3rdparty\cmft\_projects\vs2019\cmft.sln /p:Configuration=Release /p:Platform=x64
exit /B 0

:deploy_cmft
	del /Q ..\external\cmft\include\cmft\*
	copy 3rdparty\cmft\include\cmft\* ..\external\cmft\include\cmft\
	copy 3rdparty\cmft\_build\win64_vs2019\bin\cmftRelease.lib ..\external\cmft\lib\win64_vs2017\release\cmft.lib
	copy 3rdparty\cmft\_build\win64_vs2019\bin\cmftRelease.pdb ..\external\cmft\lib\win64_vs2017\release\cmft.pdb
exit /B 0

:nvtt
	cls
	echo NVTT
	echo ===============================
	echo  1. Go back
	echo  2. Download
	echo  3. Build
	echo  4. Deploy
	echo  5. Open in VS
	echo ===============================
	choice /C 12345 /N /M "Your choice:"
	if %errorlevel%==1 exit /B 0
	if %errorlevel%==2 call :download_nvtt
	if %errorlevel%==3 call :build_nvtt
	if %errorlevel%==4 call :deploy_nvtt
	if %errorlevel%==5 start "" %devenv_cmd% "3rdparty\nvtt\project\vc2017\nvtt.sln"
	pause
goto :nvtt

:deploy_nvtt
	del /Q ..\external\nvtt\include\*
	copy 3rdparty\nvtt\src\nvtt\nvtt.h ..\external\nvtt\include\
	copy 3rdparty\nvtt\project\vc2017\Release.x64\bin\nvtt.lib  ..\external\nvtt\lib\win64_vs2017\release\nvtt.lib
	copy 3rdparty\nvtt\project\vc2017\nvtt\Release\x64\nvtt.pdb  ..\external\nvtt\lib\win64_vs2017\release\nvtt.pdb
exit /B 0

:build_nvtt
	%msbuild_cmd% 3rdparty\nvtt\project\vc2017\nvtt.sln /p:Configuration=Release /p:Platform=x64
exit /B 0

:create_project
	echo Creating project...
	genie.exe --static-plugins vs2019 
	pause
exit /B 0

:build
	if not exist "tmp/vs2019/LumixEngine.sln" call :create_project
	echo Building...
	%msbuild_cmd% tmp/vs2019/LumixEngine.sln /p:Configuration=RelWithDebInfo
	pause
exit /B 0

:run_studio
	if not exist "tmp/vs2019/bin/RelWithDebInfo/studio.exe" call :build
	cd ..\data
	start "" "../projects/tmp/vs2019/bin/RelWithDebInfo/studio.exe"
	cd ..\projects
	pause
exit /B 0

:open_in_vs
	start "" %devenv_cmd% "tmp/vs2019/LumixEngine.sln"
exit /B 0

:create_bundle
	echo Creating bundle...
	genie.exe --embed-resources --static-plugins vs2019
	cd ..\data
	tar -cvf data.tar .
	move data.tar ../src/studio
	cd ..\projects\
	%msbuild_cmd% tmp/vs2019/LumixEngine.sln /p:Configuration=RelWithDebInfo
	del ..\src\studio\data.tar
	pause
exit /B 0

:download_godot
	if %RANDOM% gtr 16000 (
		start "" "https://godotengine.org/"
		echo Son, I'm disappointed.
	) else (
		echo No.
	)
	pause
exit /B 0

:download_cmft
	if not exist 3rdparty mkdir 3rdparty
	cd 3rdparty
	if not exist cmft (
		git.exe clone https://github.com/nem0/cmft.git
	) else (
		cd cmft
		git pull
		cd ..
	)
	cd ..
exit /B 0

:download_freetype
	if not exist 3rdparty mkdir 3rdparty
	cd 3rdparty
	if not exist freetype (
		git.exe clone https://github.com/aseprite/freetype2.git freetype
	) else (
		cd freetype
		git pull
		cd ..
	)
	cd ..
exit /B 0

:download_recast
	if not exist 3rdparty mkdir 3rdparty
	cd 3rdparty
	if not exist recast (
		git.exe clone https://github.com/nem0/recastnavigation.git recast
	) else (
		cd recast
		git pull
		cd ..
	)
	cd ..
exit /B 0

:download_physx
	if not exist 3rdparty mkdir 3rdparty
	cd 3rdparty
	if not exist physx (
		git.exe clone https://github.com/nem0/PhysX.git physx
	) else (
		cd physx
		git pull
		cd ..
	)
	cd ..
exit /B 0

:download_nvtt
	if not exist 3rdparty mkdir 3rdparty
	cd 3rdparty
	if not exist nvtt (
		git.exe clone https://github.com/nem0/nvidia-texture-tools.git nvtt
	) else (
		cd nvtt
		git pull
		cd ..
	)
	cd ..
exit /B 0

:open_gitter
	start "" "https://gitter.im/nem0/LumixEngine"
	pause
exit /B 0

:git_pull
	git.exe pull
exit /B 0



