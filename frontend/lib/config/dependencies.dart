import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:frontend/data/backend_plugin_provider.dart';
import 'package:frontend/state/home_page_cubit.dart';

Widget wrapWithProviders(Widget child) {
  return MultiRepositoryProvider(
    providers: [
      RepositoryProvider<BackendPluginProvider>(
        create: (_) => BackendPluginProvider(),
      ),
    ],
    child: MultiBlocProvider(
      providers: [
        BlocProvider(
          create: (context) => HomePageCubit(backend: context.read()),
        ),
      ],
      child: child,
    ),
  );
}
